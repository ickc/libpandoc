{-# LANGUAGE FlexibleContexts    #-}
{-# LANGUAGE FlexibleInstances   #-}
{-# LANGUAGE OverloadedStrings   #-}
{-# LANGUAGE RankNTypes          #-}
{-# LANGUAGE ScopedTypeVariables #-}
{-# LANGUAGE TypeOperators       #-}
{- |
   Module      : LibPandoc
   Copyright   : Copyright (C) 2026 Kolen Cheung
   License     : GNU GPL, version 2 or above

The functions behind @libpandoc.h@.

This deliberately uses only the interface upstream's own pandoc.wasm build
uses ('Opt' decoded from defaults-file JSON, 'defaultOpts',
'convertWithOpts', the Lua engine's 'getEngine') plus 'parseOptionsFromArgs'
for the argv form. Upstream keeps those working for wasm, so this module
should rarely need changes when pandoc does.

'convertWithOpts' reads files and writes a file, so stdin and stdout are
temporary files, as the wasm build uses a virtual file system.

Callback filters (@pandoc_convert_filters@) use one more public interface:
the engine's 'engineApplyFilter', which pandoc calls for every Lua filter.
A callback entry in the options becomes a Lua filter with a reserved path,
and the engine given to 'convertWithOpts' answers that path by calling the
caller's function instead of Lua.
-}
module LibPandoc () where

import Control.Exception (SomeException, bracket, fromException, displayException,
                          throwIO, try)
import Control.Monad.Except (throwError)
import Control.Monad.IO.Class (MonadIO, liftIO)
import Data.Aeson ((.=))
import qualified Data.Aeson as Aeson
import qualified Data.Aeson.KeyMap as KM
import qualified Data.Aeson.Types as Aeson (parseEither)
import qualified Data.ByteString as B
import qualified Data.ByteString.Char8 as B8
import qualified Data.ByteString.Lazy as BL
import Data.List (stripPrefix)
import Data.Maybe (fromMaybe, listToMaybe)
import Data.Scientific (toBoundedInteger)
import qualified Data.Text as T
import qualified Data.Text.Encoding as TE
import qualified Data.Text.Encoding.Error as TE
import Foreign
import Foreign.C
import GHC.Generics
import qualified GHC.Foreign as GHC
import GHC.IO.Encoding (utf8)
import System.Directory (doesFileExist)
import System.FilePath ((</>))
import System.IO.Temp (withSystemTempDirectory)
import Text.Pandoc.App (LineEnding (..), Opt (..), OptInfo (..), convertWithOpts,
                        defaultOpts,
                        options, parseOptionsFromArgs)
import Text.Pandoc.Class (PandocMonad)
import Text.Pandoc.Definition (Pandoc)
import Text.Pandoc.Error (PandocError (..), renderError)
import Text.Pandoc.Extensions (extensionsToList, showExtension)
import Text.Pandoc.Filter (Environment (..))
import qualified Text.Pandoc.Format as Format
import Text.Pandoc.Logging (Verbosity (ERROR))
import Text.Pandoc.Lua (getEngine)
import Text.Pandoc.Scripting (ScriptingEngine (..))

import LibPandoc.Query (query)
import LibPandoc.Result

foreign export ccall "libpandoc_hs_convert"
  hsConvert :: Ptr CChar -> CSize -> Ptr CChar -> CSize -> CInt -> IO (Ptr ())
foreign export ccall "libpandoc_hs_convert_args"
  hsConvertArgs :: CInt -> Ptr CString -> Ptr CChar -> CSize -> CInt -> IO (Ptr ())
foreign export ccall "libpandoc_hs_query"
  hsQuery :: Ptr CChar -> CSize -> IO (Ptr ())
foreign export ccall "libpandoc_hs_convert_filters"
  hsConvertFilters :: Ptr CChar -> CSize -> Ptr CChar -> CSize -> CInt
                   -> Ptr () -> CSize -> IO (Ptr ())

-- cbits: the buffer a callback filter answers in, and the call itself. Safe,
-- so that the callback may call libpandoc again.
foreign import ccall unsafe "libpandoc_buffer_new" bufferNew :: IO (Ptr ())
foreign import ccall unsafe "libpandoc_buffer_free" bufferFree :: Ptr () -> IO ()
foreign import ccall unsafe "libpandoc_buffer_data" bufferData :: Ptr () -> IO (Ptr CChar)
foreign import ccall unsafe "libpandoc_buffer_len" bufferLen :: Ptr () -> IO CSize
foreign import ccall safe "libpandoc_call_filter"
  callFilter :: Ptr () -> CSize -> Ptr CChar -> CSize -> Ptr CChar -> CSize
             -> Ptr () -> IO CInt

hsConvert :: Ptr CChar -> CSize -> Ptr CChar -> CSize -> CInt -> IO (Ptr ())
hsConvert optPtr optLen inPtr inLen hasIn =
  hsConvertFilters optPtr optLen inPtr inLen hasIn nullPtr 0

hsConvertFilters :: Ptr CChar -> CSize -> Ptr CChar -> CSize -> CInt
                 -> Ptr () -> CSize -> IO (Ptr ())
hsConvertFilters optPtr optLen inPtr inLen hasIn filters nFilters = respond $ do
  json <- peekBytes optPtr optLen
  input <- peekInput inPtr inLen hasIn
  let parsed = do
        v <- Aeson.eitherDecodeStrict json
        v' <- callbackFilters (fromIntegral nFilters) v
        Aeson.parseEither Aeson.parseJSON v'
  case parsed of
    Left e -> throwIO $ PandocOptionError $ T.pack e
    Right (f :: Opt -> Opt) ->
      convert (withCallbacks filters) (f defaultOpts) input

-- | The path standing for callback filter @i@: a Lua filter's, so that
-- pandoc hands it to the engine, where 'withCallbacks' catches it.
callbackPath :: Int -> FilePath
callbackPath i = callbackPrefix ++ show i

callbackPrefix :: String
callbackPrefix = "libpandoc:callback/"

-- | Replace each @{"type": "callback", "index": i}@ in the options' filters
-- by a Lua filter with 'callbackPath' @i@.
callbackFilters :: Int -> Aeson.Value -> Either String Aeson.Value
callbackFilters n (Aeson.Object o)
  | Just (Aeson.Array fs) <- KM.lookup "filters" o = do
      fs' <- traverse one fs
      pure $ Aeson.Object $ KM.insert "filters" (Aeson.Array fs') o
  where
    one (Aeson.Object f)
      | KM.lookup "type" f == Just (Aeson.String "callback") =
          case KM.lookup "index" f of
            Just (Aeson.Number x)
              | Just i <- toBoundedInteger x, i >= 0, i < n ->
                  Right $ Aeson.object [ "type" .= ("lua" :: T.Text)
                                       , "path" .= callbackPath i ]
            _ -> Left $ "a callback filter needs an \"index\" below " ++ show n
              ++ " (the number of filters given)"
    one v = Right v
callbackFilters _ v = Right v

-- | The engine, answering callback filters' paths by calling them.
withCallbacks :: Ptr () -> Opt -> ScriptingEngine -> ScriptingEngine
withCallbacks filters opts engine
  | filters == nullPtr = engine
  | otherwise = engine { engineApplyFilter = apply }
  where
    apply :: (PandocMonad m, MonadIO m)
          => Environment -> [String] -> FilePath -> Pandoc -> m Pandoc
    apply env args path doc = case stripPrefix callbackPrefix path of
      Just i | [(k, "")] <- reads i -> do
        r <- liftIO $ runCallback filters k opts env args doc
        either (throwError . PandocFilterError (T.pack ("callback " ++ show k)))
               pure r
      _ -> engineApplyFilter engine env args path doc

-- | Call callback filter @k@ on a document, as a JSON filter is run, and
-- also tell it the input and output formats (with extensions), which JSON
-- filters aren't told.
runCallback :: Ptr () -> Int -> Opt -> Environment -> [String] -> Pandoc
            -> IO (Either T.Text Pandoc)
runCallback filters k opts env args doc = bracket bufferNew bufferFree $ \buf -> do
  let docJson = BL.toStrict (Aeson.encode doc)
      context = BL.toStrict $ Aeson.encode $ Aeson.object
        [ "format" .= listToMaybe args
        , "input-format" .= inputFormat opts
        , "output-format" .= outputFormat opts
        , "reader-options" .= envReaderOptions env ]
  status <- B.useAsCStringLen docJson $ \(dp, dl) ->
    B.useAsCStringLen context $ \(cp, cl) ->
      callFilter filters (fromIntegral k) dp (fromIntegral dl)
                 cp (fromIntegral cl) buf
  p <- bufferData buf
  out <- if p == nullPtr then pure B.empty
         else B.packCStringLen . (,) p . fromIntegral =<< bufferLen buf
  pure $ if status /= 0
    then Left $ TE.decodeUtf8With TE.lenientDecode out
    else case Aeson.eitherDecodeStrict out of
      Left e -> Left $ "the filter returned an invalid document: " <> T.pack e
      Right d -> Right d

hsConvertArgs :: CInt -> Ptr CString -> Ptr CChar -> CSize -> CInt -> IO (Ptr ())
hsConvertArgs argc argv inPtr inLen hasIn = respond $ do
  args <- mapM (GHC.peekCString utf8) =<< peekArray (fromIntegral argc) argv
  input <- peekInput inPtr inLen hasIn
  parsed <- parseOptionsFromArgs options defaultOpts "pandoc" args
  case parsed of
    Right opts -> convert (const id) opts input
    Left (OptError e) -> throwIO e
    Left info -> throwIO $ PandocOptionError $
      "informational option (" <> T.pack (takeWhile (/= ' ') (show info)) <>
      ") is not supported by pandoc_convert_args; use pandoc_query"

hsQuery :: Ptr CChar -> CSize -> IO (Ptr ())
hsQuery ptr len = respond $ do
  json <- peekBytes ptr len
  out <- query json
  pure $ Result out Nothing "[]"

peekBytes :: Ptr CChar -> CSize -> IO B.ByteString
peekBytes ptr len = B.packCStringLen (ptr, fromIntegral len)

peekInput :: Ptr CChar -> CSize -> CInt -> IO (Maybe B.ByteString)
peekInput ptr len has
  | has == 0  = pure Nothing
  | otherwise = Just <$> peekBytes ptr len

-- | The input format pandoc reads with these options, as 'convertWithOpts'
-- decides it: @from@, else from the input files' names, else markdown.
inputFormat :: Opt -> T.Text
inputFormat opts = fromMaybe deduced (optFrom opts)
  where
    deduced = maybe "markdown" renderFormat $ Format.formatFromFilePaths $
      fromMaybe ["-"] (optInputFiles opts)

-- | The output format: @to@, else from the output file's name, else html.
outputFormat :: Opt -> T.Text
outputFormat opts = fromMaybe deduced (optTo opts)
  where
    deduced = maybe "html" renderFormat $ Format.formatFromFilePaths $
      maybe [] pure (optOutputFile opts)

renderFormat :: Format.FlavoredFormat -> T.Text
renderFormat (Format.FlavoredFormat name diff) =
  name <> exts "+" (Format.extsToEnable diff) <> exts "-" (Format.extsToDisable diff)
  where
    exts sign = T.concat . map ((sign <>) . showExtension) . extensionsToList

-- | Run a conversion as the pandoc CLI would, but with @input@ (if given) as
-- stdin and stdout captured, and the Lua engine adapted by @engineWith@.
convert :: (Opt -> ScriptingEngine -> ScriptingEngine) -> Opt
        -> Maybe B.ByteString -> IO Result
convert engineWith opts input = withSystemTempDirectory "libpandoc" $ \tmp -> do
  let stdinFile = tmp </> "stdin"
      -- an extension, so that zip output (chunkedhtml) is written as bytes
      -- rather than extracted into a directory, as for stdout
      stdoutFile = tmp </> "stdout.out"
      logFile = maybe (tmp </> "log.json") id (optLogFile opts)
      fromStdin = case optInputFiles opts of
                    Nothing -> True
                    Just fs -> fs == ["-"]
      toStdout = maybe True (== "-") (optOutputFile opts)
  inputFiles <- case input of
    Nothing -> pure (optInputFiles opts)
    Just bytes -> do
      B.writeFile stdinFile bytes
      pure $ Just $ maybe [stdinFile]
               (map (\f -> if f == "-" then stdinFile else f))
               (optInputFiles opts)
  let opts' = opts
        { optInputFiles = inputFiles
        , optOutputFile = if toStdout then Just stdoutFile else optOutputFile opts
        , optLogFile = Just logFile
          -- messages are returned in the log rather than printed on stderr
        , optVerbosity = ERROR
          -- captured output is an in-memory string: \n, as in Python or
          -- C text, rather than the platform's line ending; files keep
          -- pandoc's default (native)
        , optEol = case optEol opts of
            Native | toStdout -> LF
            e -> e
          -- the formats pandoc assumes for stdin and stdout, which it
          -- can't deduce from the temporary files' names
        , optFrom = case optFrom opts of
            Nothing | fromStdin && input /= Nothing -> Just "markdown"
            f -> f
        , optTo = case optTo opts of
            Nothing | toStdout -> Just "html"
            t -> t
        }
  engine <- engineWith opts' <$> getEngine
  convertWithOpts engine opts'
  out <- if toStdout then B.readFile stdoutFile else pure B.empty
  logged <- doesFileExist logFile
  logJson <- if logged then B.readFile logFile else pure "[]"
  pure $ Result out Nothing logJson

-- | Run an action and hand the result to C, turning any exception into an
-- error result: nothing may escape into the host process.
respond :: IO Result -> IO (Ptr ())
respond act = do
  r <- try act
  newResult $ case r of
    Right res -> res
    Left (e :: SomeException) -> Result B.empty (Just (describe e)) "[]"

describe :: SomeException -> (B.ByteString, B.ByteString)
describe e = case fromException e of
  Just (pe :: PandocError) ->
    (B8.pack (conNameOf pe), TE.encodeUtf8 (renderError pe))
  Nothing -> ("Exception", TE.encodeUtf8 (T.pack (displayException e)))

-- | The name of a value's constructor, e.g. "PandocParseError".
conNameOf :: (Generic a, ConName (Rep a)) => a -> String
conNameOf = conName' . from

class ConName f where
  conName' :: f p -> String
instance ConName f => ConName (D1 c f) where
  conName' (M1 x) = conName' x
instance (ConName f, ConName g) => ConName (f :+: g) where
  conName' (L1 x) = conName' x
  conName' (R1 x) = conName' x
instance Constructor c => ConName (C1 c f) where
  conName' = conName
