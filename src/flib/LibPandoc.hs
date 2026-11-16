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
which pandoc calls for every Lua filter. Three kinds of filter become Lua
filters with reserved paths, which the engine given to 'convertWithOpts'
answers itself:

* callback filters (@pandoc_convert_filters@): by calling the caller's
  function;
* JSON filters: by running them as pandoc does ('runJSONFilter'), but also
  telling them the input and output formats, in @PANDOC_INPUT_FORMAT@ and
  @PANDOC_OUTPUT_FORMAT@, as proposed upstream (jgm/pandoc#11016);
* citeproc, with the sandbox on: by running it inside pandoc's sandbox
  ('citeprocSandboxed'), which pandoc itself doesn't do.
-}
module LibPandoc () where

import qualified Control.Exception as E
import Control.Exception (Exception (..), SomeAsyncException, SomeException (..),
                          asyncExceptionFromException, asyncExceptionToException,
                          bracket, evaluate, fromException, displayException,
                          throwIO, try)
import Control.Concurrent (forkFinally, forkIOWithUnmask, killThread, mkWeakThreadId,
                           myThreadId, rtsSupportsBoundThreads, setNumCapabilities,
                           threadDelay, throwTo)
import Data.Unique (Unique, newUnique)
import GHC.Clock (getMonotonicTime)
import System.Mem.Weak (deRefWeak)
import Control.Monad (forM_, unless, when, (<=<))
import GHC.Conc (getNumProcessors)
import Control.Concurrent.MVar (MVar, newEmptyMVar, newMVar, putMVar, takeMVar, withMVar)
import Data.IORef (IORef, newIORef, readIORef, writeIORef)
import System.IO.Unsafe (unsafePerformIO)
import Control.Monad.Except (catchError, throwError)
import Control.Monad.IO.Class (MonadIO, liftIO)
import Data.Aeson ((.=))
import qualified Data.Aeson as Aeson
import qualified Data.Aeson.KeyMap as KM
import qualified Data.Aeson.Types as Aeson (parseEither)
import qualified Data.ByteString as B
import qualified Data.ByteString.Char8 as B8
import qualified Data.ByteString.Lazy as BL
import qualified Data.ByteString.Unsafe as BU
import Data.Char (toLower)
import Data.List (intersperse, stripPrefix)
import Data.Maybe (fromMaybe, isNothing, listToMaybe, maybeToList)
import Data.Scientific (FPFormat (Fixed), formatScientific, fromFloatDigits, toBoundedInteger,
                        toRealFloat)
import qualified Data.Text as T
import qualified Data.Text.Encoding as TE
import qualified Data.Text.Encoding.Error as TE
import Foreign
import Foreign.C
import GHC.Generics
import qualified GHC.Foreign as GHC
import GHC.IO.Encoding (TextEncoding, getFileSystemEncoding, utf8)
import System.Directory (createDirectory, doesDirectoryExist, doesFileExist, executable,
                         findExecutable, getPermissions, listDirectory, pathIsSymbolicLink,
                         removeDirectory, removeFile)
import System.Environment (getEnvironment, lookupEnv, withProgName)
import System.Exit (ExitCode (..))
import System.FilePath ((</>), takeBaseName, takeExtension)
import System.IO (hClose, hFlush, hPutStrLn, stderr, stdout)
import System.IO.Error (ioeGetHandle, ioeSetLocation, isResourceVanishedError, modifyIOError)
import System.IO.Temp (createTempDirectory, getCanonicalTemporaryDirectory, withSystemTempFile)
import Text.Pandoc.App (LineEnding (..), Opt (..), OptInfo (..),
                        convertWithOpts, handleOptInfo,
                        defaultOpts,
                        options, parseOptionsFromArgs, versionInfo)
import qualified Data.Set as Set
import Text.Pandoc.Data (readDataFile)
import Text.Pandoc.Citeproc (processCitations)
import Text.Pandoc.Class (PandocIO, PandocMonad, findFileWithDataFallback,
                          getLog, readFileStrict, runIO, runIOorExplode, runPure,
                          sandbox, setResourcePath, setUserDataDir, setVerbosity)
import Text.Pandoc.Options (ReaderOptions (..), def)
import Text.Pandoc.Readers (Reader (..), getReader)
import Text.Pandoc.Shared (tabFilter)
import Text.Pandoc.Definition (Pandoc)
import Text.Pandoc.Error (PandocError (..), handleError, renderError)
import Text.Pandoc.Extensions (Extension (Ext_citations), disableExtension,
                               enableExtension, extensionEnabled,
                               extensionsToList, showExtension)
import Text.Pandoc.Filter (Environment (..), Filter (..))
import qualified Text.Pandoc.Format as Format
import Text.Pandoc.Logging (LogMessage, Verbosity (ERROR), encodeLogMessages)
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
import LibPandoc.Untrusted (Call (..), untrusted, untrustedQuery)
import LibPandoc.WriterOptions (writerOptionsJSON, writerOptionsVariable)
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
        v0 <- Aeson.eitherDecodeStrict json
        (limit, v1) <- timeLimit v0
        -- untrusted code doesn't read the process's stdin
        when (isUntrusted v1 && isNothing input) $
          Left "untrusted code gives pandoc_convert its input"
        v <- untrusted Convert v1
        v' <- callbackFilters (fromIntegral nFilters) v
        (,,) limit (isUntrusted v1) <$> Aeson.parseEither Aeson.parseJSON v'
  case parsed of
    Left e -> throwIO $ PandocOptionError $ T.pack e
    Right (limit, noUserData, f :: Opt -> Opt) -> limited limit $
      convert (withHooks filters (fromIntegral nFilters)) noUserData (f defaultOpts) input

-- | Options with @"untrusted": true@ ("LibPandoc.Untrusted").
isUntrusted :: Aeson.Value -> Bool
isUntrusted (Aeson.Object o) = KM.lookup "untrusted" o == Just (Aeson.Bool True)
isUntrusted _ = False

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

-- | The path standing for citeproc run in the sandbox ('citeprocSandboxed').
citeprocPath :: FilePath
citeprocPath = "libpandoc:citeproc"

-- | Route JSON filters through the engine, as Lua filters with reserved
-- paths, so that 'withHooks' runs them. (Not @pandoc-citeproc@, which pandoc
-- itself looks for among the JSON filters, to warn that it's deprecated.)
-- With the sandbox on, citeproc too, where it stands among the filters.
routeFilters :: Opt -> Opt
routeFilters opts
  | optSandbox opts && CiteprocFilter `elem` optFilters opts =
      withoutCitations routed
  | otherwise = routed
  where
    routed = opts { optFilters = map route (optFilters opts) }
    route (JSONFilter f)
      | takeBaseName f /= "pandoc-citeproc" = LuaFilter (jsonPrefix ++ f)
    route CiteprocFilter | optSandbox opts = LuaFilter citeprocPath
    route f = f

-- | Citeproc as pandoc runs it, but inside pandoc's pure sandbox, with only
-- the files the caller named in the options: pandoc's @--sandbox@ runs the
-- reader and the writer so, but not citeproc (jgm/pandoc's App.hs), which
-- would otherwise read, or fetch, whatever bibliography, CSL style or
-- abbreviations a document names (in its metadata, or with LaTeX's
-- bibliography commands). Anything else is missing, as a file that
-- doesn't exist is: nothing else is read from the disk, nothing is fetched
-- (a CSL style's parent neither), and the data directory's styles aren't
-- looked in (pandoc's own are). The list is the citeproc part of what
-- pandoc's sandbox gives its reader and writer (@sandbox'@ in
-- Text.Pandoc.App.OutputSettings), so not a @bibliography@ given as
-- metadata (@-M@, a metadata file), as there.
citeprocSandboxed :: (PandocMonad m, MonadIO m) => Opt -> Pandoc -> m Pandoc
citeprocSandboxed opts = sandbox files . processCitations
  where
    files = maybeToList (optCSL opts) ++ maybeToList (optCitationAbbreviations opts)
              ++ optBibliography opts

-- | pandoc turns the writer's @citations@ extension off when citeproc is
-- among the filters (so that markdown, org and typst write the citations
-- as citeproc rendered them, not their own citation syntax:
-- jgm/pandoc#10662), looking for 'CiteprocFilter' there; routed through
-- the engine, it isn't, so the output format says it instead. Not for PDF
-- output without a format named, whose writer pandoc picks by the PDF
-- engine (typst's would write its own citations).
withoutCitations :: Opt -> Opt
withoutCitations opts = case writerFormat of
  Just (Format.FlavoredFormat name diff)
    | extensionEnabled Ext_citations
        (Format.extsSupported (Format.getExtensionsConfig name)) ->
        opts { optTo = Just $ renderFormat $ Format.FlavoredFormat name diff
                 { Format.extsToEnable = disableExtension Ext_citations (Format.extsToEnable diff)
                 , Format.extsToDisable = enableExtension Ext_citations (Format.extsToDisable diff) } }
  _ -> opts
  where
    -- as pandoc's optToOutputSettings decides it
    writerFormat = case optTo opts of
      Just "pdf" -> Nothing
      Just f -> either (const Nothing) Just (runPure (Format.parseFlavoredFormat f))
      Nothing
        | map toLower (takeExtension (fromMaybe "" (optOutputFile opts))) == ".pdf" -> Nothing
        | otherwise -> Format.formatFromFilePaths (maybeToList (optOutputFile opts))

-- | The engine, answering the reserved paths: callback filters by calling
-- them, JSON filters by running them, citeproc in the sandbox.
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
      | path == citeprocPath = citeprocSandboxed opts doc
      | otherwise = engineApplyFilter engine env args path doc

-- | Run a JSON filter as pandoc's own Text.Pandoc.Filter.JSON does (the
-- interpreter by file extension, the same environment), and also tell it
-- the input and output formats, the writer's options, as pandoc tells
-- only Lua filters ("LibPandoc.WriterOptions"), and whether pandoc's
-- sandbox is on (so that what it reads through pandoc is sandboxed too).
runJSONFilter :: Opt -> Environment -> [String] -> FilePath -> Pandoc
              -> IO (Either PandocError Pandoc)
#if defined(wasm32_HOST_ARCH)
-- WASI starts no programs: a filter that is one is the host's to run, as a
-- callback filter (and a wasm filter compiled for WASI is one)
runJSONFilter _ _ _ f _ = pure $ Left $ PandocFilterError (T.pack f)
  "libpandoc.wasm runs no programs: a JSON filter can't run inside it (a host may run it, as a callback filter)"
#else
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
               : ("PANDOC_WRITER_OPTIONS",
                  UTF8.toStringLazy (writerOptionsVariable (envWriterOptions fenv)))
               : ("PANDOC_INPUT_FORMAT", T.unpack (inputFormat opts))
               : ("PANDOC_OUTPUT_FORMAT", T.unpack (outputFormat opts))
               : ("PANDOC_SANDBOX", if optSandbox opts then "true" else "false")
               : env
      -- (not the time limit's exception, which ends the filter: pipeProcess
      -- sends it SIGTERM and reaps it)
      r <- trySync $ pipeProcess (Just env') f' args' (Aeson.encode doc)
      pure $ case r of
        Left (e :: SomeException) -> failed (T.pack (show e))
        Right (ExitSuccess, out) -> either (failed . T.pack) Right (Aeson.eitherDecode' out)
        Right (ExitFailure ec, _) ->
          failed ("Filter returned error status " <> T.pack (show ec))
#endif

-- | Call callback filter @k@ on a document, as a JSON filter is run, and
-- also tell it the input and output formats (with extensions), which JSON
-- filters aren't told, and the writer's options.
runCallback :: Ptr () -> Int -> Opt -> Environment -> [String] -> Pandoc
            -> IO (Either T.Text Pandoc)
runCallback filters k opts env args doc = bracket bufferNew bufferFree $ \buf -> do
  let docJson = BL.toStrict (Aeson.encode doc)
      context = BL.toStrict $ Aeson.encode $ Aeson.object
        [ "format" .= listToMaybe args
        , "input-format" .= inputFormat opts
        , "output-format" .= outputFormat opts
        , "reader-options" .= envReaderOptions env
        , "writer-options" .= writerOptionsJSON (envWriterOptions env)
        , "sandbox" .= optSandbox opts ]
  -- the bytes themselves, not a copy with a NUL (the header promises
  -- none): a strict ByteString's buffer is pinned, so the collector doesn't
  -- move it while fn runs (a safe call, during which other threads may
  -- collect, and fn may call libpandoc again), and it is kept alive until
  -- the call returns; fn only reads it, and only during the call
  status <- BU.unsafeUseAsCStringLen docJson $ \(dp, dl) ->
    BU.unsafeUseAsCStringLen context $ \(cp, cl) ->
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
    Right opts -> convert (withHooks filters (fromIntegral nFilters)) False opts input
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
        (optsV, o) <- flip Aeson.parseEither v $ Aeson.withObject "read_many request" $ \o ->
          (\opts -> (fromMaybe (Aeson.object []) opts, o)) <$> o Aeson..:? "options"
        (limit, optsV1) <- timeLimit optsV
        optsV' <- untrusted ReadMany optsV1
        flip Aeson.parseEither optsV' $ \_ -> do
          f <- Aeson.parseJSON optsV'
          inputs <- o Aeson..: "inputs"
          pure (limit, f defaultOpts, inputs)
  case parsed of
    Left e -> throwIO $ PandocOptionError $ T.pack e
    Right (limit, opts, inputs) -> do
      deadline <- traverse (\s -> (,) s . (+ s) <$> getMonotonicTime) limit
      (out, logs) <- readMany deadline opts inputs
      pure $ Result (B.concat ("[" : intersperse "," out ++ ["]"])) Nothing
                    (BL.toStrict (encodeLogMessages logs))

-- | The reader and its options, as 'convertWithOpts' sets them up for a
-- fragment (not standalone), then every input read with them in its own
-- thread, which also encodes the result (so that all of the work, reading
-- being lazy, happens in parallel). A failure is that input's result, as an
-- error object. pandoc's messages, as a conversion's, are returned (the
-- setup's, then each input's in order) rather than printed. With a time
-- limit (its seconds, and the deadline it makes), the call ends by the
-- deadline: the setup past it fails the call, an input not read by then
-- is a "Timeout" in its place.
readMany :: Maybe (Double, Double) -> Opt -> [T.Text] -> IO ([B.ByteString], [LogMessage])
readMany deadline opts inputs = do
  -- one text needs no threads (a filter's read_as)
  when (length inputs > 1) hsExpandThreads
  setup <- untilDeadline $ runIO $ do
    prepare
    flvrd <- Format.parseFlavoredFormat (fromMaybe "markdown" (optFrom opts))
    (reader, exts) <- getReader flvrd
    abbrevs <- readAbbreviations (optAbbreviations opts)
    setupLog <- getLog
    -- as Text.Pandoc.App.Input prepares a text input
    let spaces | optPreserveTabs opts
                 || Format.formatName flvrd `elem` ["t2t", "man", "tsv"] = 0
               | otherwise = optTabStop opts
        prepared = tabFilter spaces . T.filter (/= '\r')
    pure (setupLog, flvrd, reader, prepared, def
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
  (setupLog, flvrd, reader, prepared, ropts) <-
    either throwIO pure =<< maybe (throwIO (TimeLimitExceeded seconds)) pure setup
  -- with "sandbox", as pandoc's --sandbox: a reader reads no file (LaTeX's
  -- \input, RST's include, ...), for texts that aren't trusted
  let readOne t
        | optSandbox opts = sandbox [] $ do
            (r, _) <- getReader flvrd
            readWith r ropts (prepared t)
        | otherwise = readWith reader ropts (prepared t)
      -- the document or the error, encoded here, and the messages (also
      -- those logged before an error)
      readEncoded t = do
        (r, logs) <- either throwIO pure <=< runIO $ do
          prepare
          r <- (Right <$> readOne t) `catchError` (pure . Left)
          (,) r <$> getLog
        out <- evaluate $ BL.toStrict $ either (Aeson.encode . errorValue) Aeson.encode r
        pure (out, logs)
      -- whatever happens in the thread, its MVar gets an answer
      answer v = putMVar v <=< \case
        Right x -> pure x
        Left e -> do
          out <- tryAll (evaluate (BL.toStrict (Aeson.encode (errorValue (toPandocError e)))))
          pure (either (const failed) id out, [])
      failed = "{\"error\":{\"kind\":\"Exception\",\"message\":\"(a message that failed)\"}}"
  vars <- mapM (\t -> do
                  v <- newEmptyMVar
                  _ <- forkFinally (untilDeadline (readEncoded t)) (answer v . fmap (fromMaybe timedOut))
                  pure v) inputs
  (out, logs) <- unzip <$> mapM takeMVar vars
  pure (out, setupLog ++ concat logs)
  where
    -- the action, if it ends before the deadline
    untilDeadline :: IO a -> IO (Maybe a)
    untilDeadline act = case deadline of
      Nothing -> Just <$> act
      Just (_, end) -> do
        now <- getMonotonicTime
        if now >= end then pure Nothing else within (end - now) act
    timedOut = ( BL.toStrict $ Aeson.encode $ Aeson.object [ "error" .= Aeson.object
                   [ "kind" .= ("Timeout" :: T.Text)
                   , "message" .= timeLimitMessage "reading" seconds ] ]
               , [] )
    seconds = maybe 0 fst deadline
    prepare :: PandocIO ()
    prepare = do
      -- messages are returned in the log rather than printed on stderr
      setVerbosity ERROR
      setUserDataDir (optDataDir opts)
      setResourcePath (optResourcePath opts)
    readWith :: PandocMonad m => Reader m -> ReaderOptions -> T.Text -> m Pandoc
    readWith (TextReader r) ro t = r ro t
    readWith (ByteStringReader r) ro t = r ro (BL.fromStrict (TE.encodeUtf8 t))
    toPandocError :: SomeException -> PandocError
    toPandocError e = fromMaybe (PandocSomeError (T.pack (message e))) (fromException e)
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
      | Left e <- untrustedQuery o -> throwIO $ PandocOptionError $ T.pack e
      | KM.lookup "query" o == Just (Aeson.String "parse-args") -> parseArgsQuery o
      | KM.lookup "query" o == Just (Aeson.String "num-threads") ->
          BL.toStrict . Aeson.encode <$> numThreads
    _ -> query json
  pure $ Result out Nothing "[]"

-- | @pandoc_set_num_threads@: the number of capabilities the runtime grows
-- to (at least 1, at most one per processor: 'withinProcessors'), until it
-- has grown ('hsExpandThreads'); returns that number. Once it has grown,
-- nothing: returns the number in effect. Changing the number of
-- capabilities while conversions run hangs GHC's runtime now and then (a
-- thread stuck in 'setNumCapabilities', the others waiting for a
-- capability: 6 of 44,000 stress runs); the one growth never hung (0 of
-- 168,000). (The @<<loop>>@ the threads test met in CI is another bug,
-- which the growth doesn't change: several first conversions at once in
-- a fresh process.) dev/libpandoc-review and its sequel in the project.
hsSetNumThreads :: CInt -> IO CInt
hsSetNumThreads n = handleAll (const (pure 0)) $ withMVar capabilitiesLock $ \() -> do
  expanded <- readIORef threadsExpanded
  fromIntegral <$> if expanded then numThreads else do
    n' <- withinProcessors (fromIntegral n)
    writeIORef threadsSet (Just n')
    pure n'

-- | The runtime starts with one capability; the others come, once, when
-- first used in parallel: by 'readMany' with more than one text, or a
-- second caller while one call runs (not a call from inside it, by a
-- filter). The number is then fixed: 'numThreads' answers it from now on.
hsExpandThreads :: IO ()
hsExpandThreads = handleAll (const (pure ())) $ withMVar capabilitiesLock $ \() -> do
  done <- readIORef threadsExpanded
  unless done $ do
    n <- numThreads
    writeIORef threadsSet (Just n)
    writeIORef threadsExpanded True
    -- (not in the non-threaded runtime, which has one: wasm)
    when rtsSupportsBoundThreads $ setNumCapabilities n

-- | Held around the whole of 'hsSetNumThreads' and 'hsExpandThreads', so
-- that the number is set either before the growth, which uses it, or after
-- it, when it changes nothing. (It also kept two calls of GHC's
-- 'setNumCapabilities' apart, when the number could change at any time:
-- it reads the number of capabilities before it takes its own lock, so a
-- call made while another adds capabilities releases too few of them, and
-- the runtime hangs or crashes: GHC 9.10.3, and its master in 2026-11;
-- UPSTREAM.md in the project.) Neither body calls back into the host or
-- takes the lock again.
capabilitiesLock :: MVar ()
capabilitiesLock = unsafePerformIO (newMVar ())
{-# NOINLINE capabilitiesLock #-}

-- | The number of capabilities: as set (or fixed by the growth), else
-- @LIBPANDOC_NUM_THREADS@, else one per processor this process may use.
-- The only number 'setNumCapabilities' is given (besides the runtime's
-- first, 1).
numThreads :: IO Int
numThreads = readIORef threadsSet >>= \case
  Just n -> pure n
  Nothing -> do
    env <- lookupEnv "LIBPANDOC_NUM_THREADS"
    case reads <$> env of
      Just [(n, "")] | n > 0 -> withinProcessors n
      _ -> withinProcessors maxBound

-- | At least 1, at most the number of processors this process may use (its
-- CPU affinity, as GHC counts them). More capabilities than processors
-- help nobody, and changed while conversions ran they crashed GHC's
-- parallel garbage collector with @-qi1@ (GHC 9.10.3; UPSTREAM.md in the
-- project).
withinProcessors :: Int -> IO Int
withinProcessors n = do
  processors <- getNumProcessors
  pure (max 1 (min (max 1 processors) n))

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
      Aeson.object [ "filters" .= optFilters opts, "sandbox" .= optSandbox opts ]
    Left (OptError e) -> throwIO e
    Left info -> pure $ BL.toStrict $ Aeson.encode $
      Aeson.object [ "informational" .= takeWhile (/= ' ') (show info) ]

-- | @pandoc_main@: the pandoc command, in this process. As pandoc-cli's
-- main (parseOptionsFromArgs, then handleOptInfo or convertWithOpts, errors
-- through handleError), with its exit as the returned status rather than
-- the process's. @filters@ (JSON), if given, replaces the filters found in
-- the arguments, and may name callback filters. Nothing escapes it: an
-- exception escaping a foreign export goes to GHC's top-level handler,
-- which ends the process.
hsMain :: CInt -> Ptr CString -> Ptr CChar -> CSize -> Ptr () -> CSize -> IO CInt
hsMain argc argv fPtr fLen filters nFilters = handleAll (const (pure 1)) $ do
  progName <- newIORef "pandoc"
  r <- try $ do
    enc <- argvEncoding
    (prg, args) <- do
      all' <- mapM (GHC.peekCString enc) =<< peekArray (fromIntegral argc) argv
      pure $ case all' of
        p : as -> (p, as)
        [] -> ("pandoc", [])
    writeIORef progName prg
    mainWith prg args
  -- what pandoc wrote, as GHC's top-level handler flushes it at the end
  flushed <- try (hFlush stdout)
  _ <- tryAll (hFlush stderr)
  case r >> flushed of
    Right () -> pure 0
    Left (e :: SomeException)
      | Just ExitSuccess <- fromException e -> pure 0
      | Just (ExitFailure c) <- fromException e -> pure (fromIntegral c)
        -- standard output's reader went away: as the command, which GHC's
        -- top-level handler ends with status 0, silently
      | Just io <- fromException e, isResourceVanishedError io
      , ioeGetHandle io == Just stdout -> pure 0
      | otherwise -> do
          -- as GHC's top-level handler reports it for pandoc
          prg <- readIORef progName
          _ <- tryAll $ do
            hPutStrLn stderr (takeBaseName prg ++ ": " ++ message e)
            hFlush stderr
          pure 1
  where
    mainWith prg args = do
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
            -- as pandoc-cli's, which says what it was built with
            Left VersionInfo -> versionInfo features (Just (T.unpack (engineName engine))) ""
            -- (--help names the program by the process's name: here argv[0],
            -- set for the process while it prints; only --help reads it)
            Left Help -> withProgName prg (handleOptInfo engine Help)
            Left info -> handleOptInfo engine info
            Right opts0 -> do
              let opts = maybe opts0 (\fs -> opts0 { optFilters = fs }) override
              convertWithOpts (withHooks filters (fromIntegral nFilters) opts engine)
                              (routeFilters opts)
    -- pandoc-cli's, for what pandoc_main does: no server, pandoc lua but
    -- in the wasm build
    features =
#if defined(wasm32_HOST_ARCH)
      ["-server", "-lua"]
#else
      ["-server", "+lua"]
#endif

-- | The encoding of pandoc_main's arguments: as the command's getArgs, the
-- file system's, which gives bytes that aren't text back unchanged when
-- used as a path (a file named in Latin-1 on Linux). On Windows, where the
-- command reads its arguments as UTF-16, the header's UTF-8.
argvEncoding :: IO TextEncoding
#if defined(mingw32_HOST_OS)
argvEncoding = pure utf8
#else
argvEncoding = getFileSystemEncoding
#endif

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
-- stdin and stdout captured, filters routed through the engine
-- ('routeFilters'), and the Lua engine adapted by @engineWith@. With
-- @noUserData@ (untrusted options), the user data directory is an empty
-- one: templates and their partials, abbreviations, reference documents,
-- translations, CSL styles come from pandoc's own data. (Not those in
-- @PANDOC_DATA_DIRS@'s directories that pandoc reads before its sandbox,
-- through the process's environment: the template, its partials,
-- dzslides' template, the abbreviations. Only the environment says where
-- they are, which other threads read: the header.)
convert :: (Opt -> ScriptingEngine -> ScriptingEngine) -> Bool -> Opt
        -> Maybe B.ByteString -> IO Result
convert engineWith noUserData opts input = withTempDirectory $ \tmp -> do
  let stdinFile = tmp </> "stdin"
      -- an extension, so that zip output (chunkedhtml) is written as bytes
      -- rather than extracted into a directory, as for stdout
      stdoutFile = tmp </> "stdout.out"
      logFile = maybe (tmp </> "log.json") id (optLogFile opts)
      -- no input files are standard input, as for pandoc
      fromStdin = case optInputFiles opts of
                    Just fs | not (null fs) -> fs == ["-"]
                    _ -> True
      toStdout = maybe True (== "-") (optOutputFile opts)
      noData = tmp </> "data"
  when noUserData $ createDirectory noData
  inputFiles <- case input of
    Nothing -> pure (optInputFiles opts)
    Just bytes -> do
      files <- case optInputFiles opts of
        Just fs | not (null fs) ->
          if "-" `elem` fs then pure (map (\f -> if f == "-" then stdinFile else f) fs)
          else throwIO $ PandocOptionError
            "input is given, but the input files don't name it: add \"-\" to them, or give no input"
        _ -> pure [stdinFile]
      B.writeFile stdinFile bytes
      pure (Just files)
  let opts' = opts
        { optInputFiles = inputFiles
        , optOutputFile = if toStdout then Just stdoutFile else optOutputFile opts
        , optDataDir = if noUserData then Just noData else optDataDir opts
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
  done <- trySync (convertWithOpts engine (routeFilters opts'))
  -- the log, also of a conversion that failed after writing it (as
  -- --fail-if-warnings does, its warnings being the reason); but not the
  -- caller's own log file then, which may be an earlier conversion's
  logged <- doesFileExist logFile
  let fresh = either (const (isNothing (optLogFile opts))) (const True) done
  logJson <- if logged && fresh then B.readFile logFile else pure "[]"
  case done of
    Left (e :: SomeException) -> pure $ Result B.empty (Just (describe e)) logJson
    Right () -> do
      out <- if toStdout then B.readFile stdoutFile else pure B.empty
      pure $ Result out Nothing logJson

-- | A call's temporary directory (in TMPDIR, as temporary's
-- 'System.IO.Temp.withSystemTempDirectory' makes it), removed afterwards
-- with all it holds, also when the call fails or is stopped. Failing to
-- remove it fails the call, so that a document left behind is said, not
-- kept quietly ("Exception", naming the directory).
withTempDirectory :: (FilePath -> IO a) -> IO a
withTempDirectory = bracket
  (getCanonicalTemporaryDirectory >>= (`createTempDirectory` "libpandoc"))
  (\dir -> modifyIOError (`ioeSetLocation` ("removing the call's temporary directory " ++ dir))
                         (removeTree dir))

-- | Remove a directory and what is in it (a symbolic link, not what it
-- points to). Not the directory package's 'removeDirectoryRecursive' (which
-- temporary uses, ignoring its failure): under WASI it fails before removing
-- anything, as it @dup@s the directory's descriptor and WASI has no @dup@
-- (directory 1.3.10, unix 2.8.8), so libpandoc.wasm kept every call's
-- files.
removeTree :: FilePath -> IO ()
removeTree dir = do
  names <- listDirectory dir
  forM_ names $ \name -> do
    let p = dir </> name
    link <- pathIsSymbolicLink p
    isDir <- if link then pure False else doesDirectoryExist p
    if isDir then removeTree p else removeFile p
  removeDirectory dir

-- | Run an action and hand the result to C, turning any exception into an
-- error result: nothing may escape into the host process, as GHC's
-- top-level handler, which every foreign export runs under, would end it.
-- So the whole result is evaluated here (a lazy part of it, or of an
-- error's message, may throw too); NULL only if memory runs out.
respond :: IO Result -> IO (Ptr ())
respond act = handleAll (const (pure nullPtr)) $ do
  r <- try (forced =<< act)
  res <- case r of
    Right res -> pure res
    Left e -> do
      d <- try (forcedPair (describe e))
      pure $ Result B.empty
        (Just (either (\(_ :: SomeException) -> ("Exception", "(a message that failed)")) id d)) "[]"
  newResult res
  where
    forced (Result o e l) = Result <$> evaluate o <*> traverse forcedPair e <*> evaluate l
    forcedPair (k, m) = (,) <$> evaluate k <*> evaluate m

describe :: SomeException -> (B.ByteString, B.ByteString)
describe e = case fromException e of
  Just (pe :: PandocError) ->
    (B8.pack (conNameOf pe), TE.encodeUtf8 (renderError pe))
  Nothing
    | Just (TimeLimitExceeded s) <- fromException e ->
        ("Timeout", TE.encodeUtf8 (timeLimitMessage "the call" s))
    | otherwise -> ("Exception", TE.encodeUtf8 (T.pack (message e)))

-- | A call's time limit (libpandoc's option @"timeout"@, not pandoc's): its
-- seconds (0 or absent: none), and the options without it.
timeLimit :: Aeson.Value -> Either String (Maybe Double, Aeson.Value)
timeLimit (Aeson.Object o) = case KM.lookup "timeout" o of
  Nothing -> Right (Nothing, Aeson.Object o)
  Just (Aeson.Number x)
    | x == 0 -> Right (Nothing, rest)
    | x > 0 -> Right (Just (toRealFloat x), rest)
  Just v -> Left ("\"timeout\" is a number of seconds, 0 or more (0: none), not "
                  <> UTF8.toStringLazy (Aeson.encode v))
  where rest = Aeson.Object (KM.delete "timeout" o)
timeLimit v = Right (Nothing, v)

-- | A call that ran past its time limit.
newtype TimeLimitExceeded = TimeLimitExceeded Double
  deriving Show
instance Exception TimeLimitExceeded

timeLimitMessage :: T.Text -> Double -> T.Text
timeLimitMessage what s = what <> " took longer than its time limit (\"timeout\": "
  <> T.pack (formatScientific Fixed Nothing (fromFloatDigits s)) <> " s)"

-- | A call under its time limit, if any: past it, the call is stopped and
-- fails with 'TimeLimitExceeded' (its temporary files removed, a JSON
-- filter it runs terminated). Also a call that failed once its time was up:
-- pandoc's code or a library may have caught the exception, and made an
-- error of its own of it.
limited :: Maybe Double -> IO Result -> IO Result
limited Nothing act = act
limited (Just s) act = do
  start <- getMonotonicTime
  r <- within s act
  end <- getMonotonicTime
  case r of
    Just res | isNothing (resultError res) || end - start < s -> pure res
    _ -> throwIO (TimeLimitExceeded s)

-- | System.Timeout's 'System.Timeout.timeout' (as in base 4.6, before its
-- event manager variant), but holding the thread it stops only by a weak
-- reference: a thread that nothing else can wake (a deadlock; GHC 9.10's
-- lost wakeup, cbits/libpandoc.c) is then still found unreachable by the
-- garbage collector and fails at once (BlockedIndefinitely..., NonTermination),
-- rather than waiting for the limit. Not in a foreign call (a callback
-- filter, Lua): the exception arrives when the call returns.
within :: Double -> IO a -> IO (Maybe a)
within s act = do
  me <- mkWeakThreadId =<< myThreadId
  u <- newUnique
  let micros | s * 1e6 >= fromIntegral (maxBound :: Int) = maxBound
             | otherwise = max 1 (ceiling (s * 1e6))
      stop = deRefWeak me >>= mapM_ (`throwTo` TimeUp u)
  E.handleJust (\(TimeUp u') -> if u == u' then Just () else Nothing) (\() -> pure Nothing) $
    bracket (forkIOWithUnmask $ \unmask -> unmask (threadDelay micros >> stop))
            (E.uninterruptibleMask_ . killThread)
            (\_ -> Just <$> act)

-- | The exception 'within' stops a thread with: asynchronous, so that
-- 'trySync' doesn't take it for the conversion's own failure.
newtype TimeUp = TimeUp Unique
instance Show TimeUp where show _ = "<<time limit>>"
instance Exception TimeUp where
  toException = asyncExceptionToException
  fromException = asyncExceptionFromException

-- | 'try', but not for asynchronous exceptions (a time limit, a thread
-- killed), which aren't the action's failure: they go on.
trySync :: IO a -> IO (Either SomeException a)
trySync act = try act >>= \case
  Left e | Just (_ :: SomeAsyncException) <- fromException e -> throwIO e
  r -> pure r

-- | An exception's message, without the context GHC adds to it from 9.10
-- (a backtrace of the runtime's own code, which tells a reader nothing).
message :: SomeException -> String
message (SomeException e) = displayException e

-- | Catch everything, asynchronous exceptions too: for the outermost
-- layer of what a foreign export runs.
tryAll :: IO a -> IO (Either SomeException a)
tryAll = try

handleAll :: (SomeException -> IO a) -> IO a -> IO a
handleAll = E.handle

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
