{-# LANGUAGE CPP                 #-}
{-# LANGUAGE FlexibleContexts    #-}
{-# LANGUAGE LambdaCase          #-}
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

Filters use one more public interface: the engine's 'engineApplyFilter',
which pandoc calls for every Lua filter. Two kinds of filter become Lua
filters with reserved paths, which the engine given to 'convertWithOpts'
answers itself:

* callback filters (@pandoc_convert_filters@): by calling the caller's
  function;
* JSON filters: by running them as pandoc does ('runJSONFilter'), but also
  telling them the input and output formats, in @PANDOC_INPUT_FORMAT@ and
  @PANDOC_OUTPUT_FORMAT@, as proposed upstream (jgm/pandoc#11016).
-}
module LibPandoc () where

import qualified Control.Exception as E
import Control.Exception (SomeException, bracket, fromException, displayException,
                          throwIO, try)
import Control.Concurrent (forkIO, rtsSupportsBoundThreads, setNumCapabilities)
import Control.Monad (unless, when, (<=<))
import GHC.Conc (getNumProcessors)
import Control.Concurrent.MVar (newEmptyMVar, putMVar, takeMVar)
import Data.IORef (IORef, atomicModifyIORef', newIORef, readIORef, writeIORef)
import System.IO.Unsafe (unsafePerformIO)
import Control.Monad.Except (throwError)
import Control.Monad.IO.Class (MonadIO, liftIO)
import Data.Aeson ((.=))
import qualified Data.Aeson as Aeson
import qualified Data.Aeson.KeyMap as KM
import qualified Data.Aeson.Types as Aeson (parseEither)
import qualified Data.ByteString as B
import qualified Data.ByteString.Char8 as B8
import qualified Data.ByteString.Lazy as BL
import Data.Char (toLower)
import Data.List (intersperse, stripPrefix)
import Data.Maybe (fromMaybe, isNothing, listToMaybe)
import Data.Scientific (toBoundedInteger)
import qualified Data.Text as T
import qualified Data.Text.Encoding as TE
import qualified Data.Text.Encoding.Error as TE
import Foreign
import Foreign.C
import GHC.Generics
import qualified GHC.Foreign as GHC
import GHC.IO.Encoding (utf8)
import System.Directory (doesFileExist, executable, findExecutable, getPermissions)
import System.Environment (getEnvironment, lookupEnv)
import System.Exit (ExitCode (..))
import System.FilePath ((</>), takeBaseName, takeExtension)
import System.IO (hClose, hFlush, hPutStrLn, stderr, stdout)
import System.IO.Temp (withSystemTempDirectory, withSystemTempFile)
import Text.Pandoc.App (Filter (..), LineEnding (..), Opt (..), OptInfo (..),
                        convertWithOpts, handleOptInfo,
                        defaultOpts,
                        options, parseOptionsFromArgs)
import qualified Data.Set as Set
import Text.Pandoc.Data (readDataFile)
import Text.Pandoc.Class (PandocIO, PandocMonad, findFileWithDataFallback,
                          readFileStrict, runIO, runIOorExplode, sandbox, setResourcePath,
                          setUserDataDir)
import Text.Pandoc.Options (ReaderOptions (..), def)
import Text.Pandoc.Readers (Reader (..), getReader)
import Text.Pandoc.Shared (tabFilter)
import Text.Pandoc.Definition (Pandoc)
import Text.Pandoc.Error (PandocError (..), handleError, renderError)
import Text.Pandoc.Extensions (extensionsToList, showExtension)
import Text.Pandoc.Filter (Environment (..))
import qualified Text.Pandoc.Format as Format
import Text.Pandoc.Logging (Verbosity (ERROR))
#if defined(wasm32_HOST_ARCH)
import Text.Pandoc.Lua (getEngine)
#else
import Text.Pandoc.Lua (getEngine, runLua, runLuaNoEnv)
#endif
#if !defined(wasm32_HOST_ARCH)
import HsLua.CLI (EnvBehavior (..), Settings (..), runStandalone)
#endif
import Text.Pandoc.Process (pipeProcess)
import Text.Pandoc.Scripting (ScriptingEngine (..))
import qualified Text.Pandoc.UTF8 as UTF8
import Text.Pandoc.Version (pandocVersionText)

import LibPandoc.Query (query)
import LibPandoc.Result

foreign export ccall "libpandoc_hs_convert"
  hsConvert :: Ptr CChar -> CSize -> Ptr CChar -> CSize -> CInt -> IO (Ptr ())
foreign export ccall "libpandoc_hs_convert_args"
  hsConvertArgs :: CInt -> Ptr CString -> Ptr CChar -> CSize -> CInt -> IO (Ptr ())
foreign export ccall "libpandoc_hs_query"
  hsQuery :: Ptr CChar -> CSize -> IO (Ptr ())
foreign export ccall "libpandoc_hs_convert_args_filters"
  hsConvertArgsFilters :: CInt -> Ptr CString -> Ptr CChar -> CSize -> CInt
                       -> Ptr () -> CSize -> IO (Ptr ())
foreign export ccall "libpandoc_hs_expand_threads"
  hsExpandThreads :: IO ()
foreign export ccall "libpandoc_hs_set_num_threads"
  hsSetNumThreads :: CInt -> IO CInt
foreign export ccall "libpandoc_hs_main"
  hsMain :: CInt -> Ptr CString -> Ptr CChar -> CSize -> Ptr () -> CSize -> IO CInt
foreign export ccall "libpandoc_hs_read_many"
  hsReadMany :: Ptr CChar -> CSize -> IO (Ptr ())
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
      convert (withHooks filters (fromIntegral nFilters)) (f defaultOpts) input

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

-- | The path standing for JSON filter @f@, which 'withHooks' runs.
jsonPrefix :: String
jsonPrefix = "libpandoc:json/"

-- | Route JSON filters through the engine, as Lua filters with reserved
-- paths, so that 'withHooks' runs them. (Not @pandoc-citeproc@, which pandoc
-- itself looks for among the JSON filters, to warn that it's deprecated.)
routeJSONFilters :: Opt -> Opt
routeJSONFilters opts = opts { optFilters = map route (optFilters opts) }
  where
    route (JSONFilter f)
      | takeBaseName f /= "pandoc-citeproc" = LuaFilter (jsonPrefix ++ f)
    route f = f

-- | The engine, answering the reserved paths: callback filters by calling
-- them, JSON filters by running them.
withHooks :: Ptr () -> Int -> Opt -> ScriptingEngine -> ScriptingEngine
withHooks filters n opts engine = engine { engineApplyFilter = apply }
  where
    apply :: (PandocMonad m, MonadIO m)
          => Environment -> [String] -> FilePath -> Pandoc -> m Pandoc
    apply env args path doc
      | Just i <- stripPrefix callbackPrefix path
      , [(k, "")] <- reads i, k >= 0, k < n, filters /= nullPtr = do
          r <- liftIO $ runCallback filters k opts env args doc
          either (throwError . PandocFilterError (T.pack ("callback " ++ show k)))
                 pure r
      | Just f <- stripPrefix jsonPrefix path = do
          -- as pandoc's expandFilterPath does for JSON filters
          f' <- fromMaybe f <$> findFileWithDataFallback "filters" f
          liftIO (runJSONFilter opts env args f' doc) >>= either throwError pure
      | otherwise = engineApplyFilter engine env args path doc

-- | Run a JSON filter as pandoc's own Text.Pandoc.Filter.JSON does (the
-- interpreter by file extension, the same environment), and also tell it
-- the input and output formats.
runJSONFilter :: Opt -> Environment -> [String] -> FilePath -> Pandoc
              -> IO (Either PandocError Pandoc)
runJSONFilter opts fenv args f doc = do
  exists <- doesFileExist f
  isExecutable <- if exists
                     then executable <$> getPermissions f
                     else return True
  let (f', args') = if exists
        then case map toLower (takeExtension f) of
               _      | isExecutable -> ("." </> f, args)
               ".py"  -> ("python", f:args)
               ".hs"  -> ("runhaskell", f:args)
               ".pl"  -> ("perl", f:args)
               ".rb"  -> ("ruby", f:args)
               ".php" -> ("php", f:args)
               ".js"  -> ("node", f:args)
               ".r"   -> ("Rscript", f:args)
               _      -> (f, args)
        else (f, args)
      failed = Left . PandocFilterError (T.pack f)
  mbExe <- if exists && isExecutable then pure (Just f') else findExecutable f'
  if isNothing mbExe
    then pure $ failed $ T.pack $ "Could not find executable " <> f'
    else do
      env <- getEnvironment
      let env' = ("PANDOC_VERSION", T.unpack pandocVersionText)
               : ("PANDOC_READER_OPTIONS",
                  UTF8.toStringLazy (Aeson.encode (envReaderOptions fenv)))
               : ("PANDOC_INPUT_FORMAT", T.unpack (inputFormat opts))
               : ("PANDOC_OUTPUT_FORMAT", T.unpack (outputFormat opts))
               : env
      r <- try $ pipeProcess (Just env') f' args' (Aeson.encode doc)
      pure $ case r of
        Left (e :: SomeException) -> failed (T.pack (show e))
        Right (ExitSuccess, out) -> either (failed . T.pack) Right (Aeson.eitherDecode' out)
        Right (ExitFailure ec, _) ->
          failed ("Filter returned error status " <> T.pack (show ec))

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
hsConvertArgs argc argv inPtr inLen hasIn =
  hsConvertArgsFilters argc argv inPtr inLen hasIn nullPtr 0

-- | The argv form with callback filters, named in the arguments as the Lua
-- filters @--lua-filter=libpandoc:callback/i@.
hsConvertArgsFilters :: CInt -> Ptr CString -> Ptr CChar -> CSize -> CInt
                     -> Ptr () -> CSize -> IO (Ptr ())
hsConvertArgsFilters argc argv inPtr inLen hasIn filters nFilters = respond $ do
  args <- mapM (GHC.peekCString utf8) =<< peekArray (fromIntegral argc) argv
  input <- peekInput inPtr inLen hasIn
  parsed <- parseOptionsFromArgs options defaultOpts "pandoc" args
  case parsed of
    Right opts -> convert (withHooks filters (fromIntegral nFilters)) opts input
    Left (OptError e) -> throwIO e
    Left info -> throwIO $ PandocOptionError $
      "informational option (" <> T.pack (takeWhile (/= ' ') (show info)) <>
      ") is not supported by pandoc_convert_args; use pandoc_query"

-- | Read many texts, each on its own, in parallel (@pandoc_read_many@).
hsReadMany :: Ptr CChar -> CSize -> IO (Ptr ())
hsReadMany ptr len = respond $ do
  json <- peekBytes ptr len
  let parsed = do
        v <- Aeson.eitherDecodeStrict json
        flip Aeson.parseEither v $ Aeson.withObject "read_many request" $ \o -> do
          optsV <- fromMaybe (Aeson.object []) <$> o Aeson..:? "options"
          f <- Aeson.parseJSON optsV
          inputs <- o Aeson..: "inputs"
          pure (f defaultOpts, inputs)
  case parsed of
    Left e -> throwIO $ PandocOptionError $ T.pack e
    Right (opts, inputs) -> do
      out <- readMany opts inputs
      pure $ Result (B.concat ("[" : intersperse "," out ++ ["]"])) Nothing "[]"

-- | The reader and its options, as 'convertWithOpts' sets them up for a
-- fragment (not standalone), then every input read with them in its own
-- thread, which also encodes the result (so that all of the work, reading
-- being lazy, happens in parallel). A failure is that input's result, as an
-- error object.
readMany :: Opt -> [T.Text] -> IO [B.ByteString]
readMany opts inputs = do
  -- one text needs no threads (a filter's read_as)
  when (length inputs > 1) hsExpandThreads
  setup <- runIO $ do
    prepare
    flvrd <- Format.parseFlavoredFormat (fromMaybe "markdown" (optFrom opts))
    (reader, exts) <- getReader flvrd
    abbrevs <- readAbbreviations (optAbbreviations opts)
    -- as Text.Pandoc.App.Input prepares a text input
    let spaces | optPreserveTabs opts
                 || Format.formatName flvrd `elem` ["t2t", "man", "tsv"] = 0
               | otherwise = optTabStop opts
        prepared = tabFilter spaces . T.filter (/= '\r')
    pure (flvrd, reader, prepared, def
      { readerColumns = optColumns opts
      , readerTabStop = optTabStop opts
      , readerIndentedCodeClasses = optIndentedCodeClasses opts
      , readerDefaultImageExtension = optDefaultImageExtension opts
      , readerTrackChanges = optTrackChanges opts
      , readerAbbreviations = abbrevs
      , readerExtensions = exts
      , readerStripComments = optStripComments opts
      , readerTypstInputs = optTypstInputs opts
      })
  (flvrd, reader, prepared, ropts) <- either throwIO pure setup
  -- with "sandbox", as pandoc's --sandbox: a reader reads no file (LaTeX's
  -- \input, RST's include, ...), for texts that aren't trusted
  let readOne t
        | optSandbox opts = sandbox [] $ do
            (r, _) <- getReader flvrd
            readWith r ropts (prepared t)
        | otherwise = readWith reader ropts (prepared t)
  vars <- mapM (\t -> do
                  v <- newEmptyMVar
                  _ <- forkIO $ do
                    r <- try (runIO (prepare >> readOne t))
                    let value = either (errorValue . toPandocError) (either errorValue Aeson.toJSON) r
                    putMVar v $! BL.toStrict (Aeson.encode value)
                  pure v) inputs
  mapM takeMVar vars
  where
    prepare :: PandocIO ()
    prepare = do
      setUserDataDir (optDataDir opts)
      setResourcePath (optResourcePath opts)
    readWith :: PandocMonad m => Reader m -> ReaderOptions -> T.Text -> m Pandoc
    readWith (TextReader r) ro t = r ro t
    readWith (ByteStringReader r) ro t = r ro (BL.fromStrict (TE.encodeUtf8 t))
    toPandocError :: SomeException -> PandocError
    toPandocError e = fromMaybe (PandocSomeError (T.pack (displayException e))) (fromException e)
    errorValue :: PandocError -> Aeson.Value
    errorValue e = Aeson.object
      [ "error" .= Aeson.object [ "kind" .= conNameOf e, "message" .= renderError e ] ]

-- | As pandoc's own (Text.Pandoc.App), which isn't exported.
readAbbreviations :: PandocMonad m => Maybe FilePath -> m (Set.Set T.Text)
readAbbreviations mbfilepath =
  Set.fromList . filter (not . T.null) . T.lines . TE.decodeUtf8With TE.lenientDecode <$>
    maybe (readDataFile "abbreviations") readFileStrict mbfilepath

hsQuery :: Ptr CChar -> CSize -> IO (Ptr ())
hsQuery ptr len = respond $ do
  json <- peekBytes ptr len
  out <- case Aeson.decodeStrict json of
    Just (Aeson.Object o)
      | KM.lookup "query" o == Just (Aeson.String "parse-args") -> parseArgsQuery o
      | KM.lookup "query" o == Just (Aeson.String "num-threads") ->
          BL.toStrict . Aeson.encode <$> numThreads
    _ -> query json
  pure $ Result out Nothing "[]"

-- | @pandoc_set_num_threads@: the number of capabilities, from now on
-- (at least 1); returns the new number.
hsSetNumThreads :: CInt -> IO CInt
hsSetNumThreads n = do
  let n' = max 1 (fromIntegral n)
  writeIORef threadsSet (Just n')
  expanded <- readIORef threadsExpanded
  when (expanded && rtsSupportsBoundThreads) $ setNumCapabilities n'
  pure (fromIntegral n')

-- | The runtime starts with one capability; the others come when first
-- used in parallel: by 'readMany' with more than one text, or a second
-- caller while one call runs (not a call from inside it, by a filter).
hsExpandThreads :: IO ()
hsExpandThreads = do
  done <- atomicModifyIORef' threadsExpanded (\d -> (True, d))
  -- (not in the non-threaded runtime, which has one: wasm)
  unless (done || not rtsSupportsBoundThreads) $ setNumCapabilities =<< numThreads

-- | The number of capabilities to use: as set, else
-- @LIBPANDOC_NUM_THREADS@, else one per processor this process may use.
numThreads :: IO Int
numThreads = readIORef threadsSet >>= \case
  Just n -> pure n
  Nothing -> do
    env <- lookupEnv "LIBPANDOC_NUM_THREADS"
    case reads <$> env of
      Just [(n, "")] | n > 0 -> pure n
      _ -> getNumProcessors

threadsSet :: IORef (Maybe Int)
threadsSet = unsafePerformIO (newIORef Nothing)
{-# NOINLINE threadsSet #-}

threadsExpanded :: IORef Bool
threadsExpanded = unsafePerformIO (newIORef False)
{-# NOINLINE threadsExpanded #-}

-- | @{"query": "parse-args", "args": [...]}@: what pandoc makes of these
-- command-line arguments (defaults files included): the filters it would
-- run, or that they ask for information (@--version@, @--list-*@, ...).
parseArgsQuery :: Aeson.Object -> IO B.ByteString
parseArgsQuery o = do
  args <- case KM.lookup "args" o of
    Just v | Aeson.Success xs <- Aeson.fromJSON v -> pure (xs :: [String])
    _ -> throwIO $ PandocOptionError "parse-args needs \"args\", a list of strings"
  case subcommand args of
    Just sub -> pure $ BL.toStrict $ Aeson.encode $ Aeson.object [ "subcommand" .= sub ]
    Nothing -> parseArgs args

parseArgs :: [String] -> IO B.ByteString
parseArgs args = do
  parsed <- parseOptionsFromArgs options defaultOpts "pandoc" args
  case parsed of
    Right opts -> pure $ BL.toStrict $ Aeson.encode $
      Aeson.object [ "filters" .= optFilters opts ]
    Left (OptError e) -> throwIO e
    Left info -> pure $ BL.toStrict $ Aeson.encode $
      Aeson.object [ "informational" .= takeWhile (/= ' ') (show info) ]

-- | @pandoc_main@: the pandoc command, in this process. As pandoc-cli's
-- main (parseOptionsFromArgs, then handleOptInfo or convertWithOpts, errors
-- through handleError), with its exit as the returned status rather than
-- the process's. @filters@ (JSON), if given, replaces the filters found in
-- the arguments, and may name callback filters.
hsMain :: CInt -> Ptr CString -> Ptr CChar -> CSize -> Ptr () -> CSize -> IO CInt
hsMain argc argv fPtr fLen filters nFilters = do
  (prg, args) <- do
    all' <- mapM (GHC.peekCString utf8) =<< peekArray (fromIntegral argc) argv
    pure $ case all' of
      p : as -> (p, as)
      [] -> ("pandoc", [])
  r <- try $ do
    override <- if fPtr == nullPtr then pure Nothing else do
      json <- peekBytes fPtr fLen
      let parsed = do
            v <- Aeson.eitherDecodeStrict json
            wrapped <- callbackFilters (fromIntegral nFilters)
                         (Aeson.object ["filters" .= (v :: Aeson.Value)])
            case wrapped of
              Aeson.Object o | Just fs <- KM.lookup "filters" o -> Aeson.parseEither Aeson.parseJSON fs
              _ -> Left "filters: expected a list"
      either (throwIO . PandocOptionError . T.pack) (pure . Just) parsed
    E.handle (handleError . Left) $ case (takeBaseName prg, subcommand args) of
     ("pandoc-lua", _) -> runLuaInterpreter prg args
     (_, Just "lua") -> runLuaInterpreter (prg ++ " lua") (drop 1 args)
     (_, Just _) -> do
      hPutStrLn stderr $ "Server mode unsupported.\n" <>
                         "libpandoc runs pandoc in process; run pandoc server for a server."
      E.throwIO (ExitFailure 4)
     _ -> do
      engine <- getEngine
      res <- parseOptionsFromArgs options defaultOpts prg args
      case res of
        Left info -> handleOptInfo engine info
        Right opts0 -> do
          let opts = maybe opts0 (\fs -> opts0 { optFilters = fs }) override
          convertWithOpts (withHooks filters (fromIntegral nFilters) opts engine)
                          (routeJSONFilters opts)
  hFlush stdout
  hFlush stderr
  case r of
    Right () -> pure 0
    Left (e :: SomeException)
      | Just ExitSuccess <- fromException e -> pure 0
      | Just (ExitFailure c) <- fromException e -> pure (fromIntegral c)
      | otherwise -> do
          -- as GHC's top-level handler reports it for pandoc
          hPutStrLn stderr (takeBaseName prg ++ ": " ++ displayException e)
          hFlush stderr
          pure 1

-- | pandoc's subcommands: @lua@ and @server@, as the first argument.
subcommand :: [String] -> Maybe String
subcommand (a : _) | a `elem` ["lua", "server"] = Just a
subcommand _ = Nothing

-- | @pandoc lua@: pandoc as a Lua interpreter, compatible with @lua@. From
-- pandoc-cli's PandocCLI.Lua (GPL-2.0-or-later, © 2022-2024 Albert
-- Krewinkel), which is in no library. The REPL's history goes to
-- @PANDOC_REPL_HISTORY@, else to a temporary file.
runLuaInterpreter :: String -> [String] -> IO ()
#if defined(wasm32_HOST_ARCH)
-- no REPL in the wasm build (hslua-cli isn't built for it), as in pandoc.wasm
runLuaInterpreter progName _ = do
  hPutStrLn stderr $ progName <> ": the Lua interpreter is not in libpandoc.wasm"
  E.throwIO (ExitFailure 4)
#else
runLuaInterpreter progName args = do
  mbhistfile <- lookupEnv "PANDOC_REPL_HISTORY"
  case mbhistfile of
    Just histfile -> runWithHistory histfile
    Nothing -> withSystemTempFile "pandoc-hist" $ \fp handle -> do
      hClose handle
      runWithHistory fp
  where
    runWithHistory histfile =
      runStandalone Settings
        { settingsVersionInfo =
            "\nEmbedded in pandoc " <> pandocVersionText <>
            "  Copyright (C) 2006-2024 John MacFarlane"
        , settingsRunner = runner
        , settingsHistory = Just histfile
        } progName args
    runner envBehavior =
      let runLua' = case envBehavior of
                      IgnoreEnvVars  -> runLuaNoEnv
                      ConsultEnvVars -> runLua
      in handleError <=< runIOorExplode . runLua'
#endif

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
-- stdin and stdout captured, JSON filters routed through the engine, and
-- the Lua engine adapted by @engineWith@.
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
  convertWithOpts engine (routeJSONFilters opts')
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
