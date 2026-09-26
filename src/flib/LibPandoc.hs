{-# LANGUAGE FlexibleContexts    #-}
{-# LANGUAGE FlexibleInstances   #-}
{-# LANGUAGE OverloadedStrings   #-}
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
-}
module LibPandoc () where

import Control.Exception (SomeException, fromException, displayException,
                          throwIO, try)
import qualified Data.Aeson as Aeson
import qualified Data.ByteString as B
import qualified Data.ByteString.Char8 as B8
import qualified Data.Text as T
import qualified Data.Text.Encoding as TE
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
import Text.Pandoc.Error (PandocError (..), renderError)
import Text.Pandoc.Logging (Verbosity (ERROR))
import Text.Pandoc.Lua (getEngine)

import LibPandoc.Query (query)
import LibPandoc.Result

foreign export ccall "libpandoc_hs_convert"
  hsConvert :: Ptr CChar -> CSize -> Ptr CChar -> CSize -> CInt -> IO (Ptr ())
foreign export ccall "libpandoc_hs_convert_args"
  hsConvertArgs :: CInt -> Ptr CString -> Ptr CChar -> CSize -> CInt -> IO (Ptr ())
foreign export ccall "libpandoc_hs_query"
  hsQuery :: Ptr CChar -> CSize -> IO (Ptr ())

hsConvert :: Ptr CChar -> CSize -> Ptr CChar -> CSize -> CInt -> IO (Ptr ())
hsConvert optPtr optLen inPtr inLen hasIn = respond $ do
  json <- peekBytes optPtr optLen
  input <- peekInput inPtr inLen hasIn
  case Aeson.eitherDecodeStrict json of
    Left e -> throwIO $ PandocOptionError $ T.pack e
    Right (f :: Opt -> Opt) -> convert (f defaultOpts) input

hsConvertArgs :: CInt -> Ptr CString -> Ptr CChar -> CSize -> CInt -> IO (Ptr ())
hsConvertArgs argc argv inPtr inLen hasIn = respond $ do
  args <- mapM (GHC.peekCString utf8) =<< peekArray (fromIntegral argc) argv
  input <- peekInput inPtr inLen hasIn
  parsed <- parseOptionsFromArgs options defaultOpts "pandoc" args
  case parsed of
    Right opts -> convert opts input
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

-- | Run a conversion as the pandoc CLI would, but with @input@ (if given) as
-- stdin and stdout captured.
convert :: Opt -> Maybe B.ByteString -> IO Result
convert opts input = withSystemTempDirectory "libpandoc" $ \tmp -> do
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
  engine <- getEngine
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
