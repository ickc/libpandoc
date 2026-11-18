{-# LANGUAGE FlexibleContexts     #-}
{-# LANGUAGE LambdaCase          #-}
{-# LANGUAGE OverloadedStrings   #-}
{-# LANGUAGE RankNTypes          #-}
{-# LANGUAGE ScopedTypeVariables #-}
{- |
   Module      : LibPandoc.Panclude
   Copyright   : Copyright (C) 2026 Kolen Cheung
   License     : GNU GPL, version 2 or above

panclude, the include step built into libpandoc: the @include@ attribute
on code blocks and divs, as the Lua filter @panclude.lua@ does it
(<https://github.com/ickc/panclude>, whose SPEC.md and cases are the
contract), with what a Lua filter can't do: it reads only inside its
/roots/, directories judged after symbolic links are resolved, and knows the
main reader exactly.

The roots: the main document's directory (its first input file's; the
working directory for standard input) unless the filter entry names others
(@{"type": "panclude", "roots": [...]}@). Under pandoc's sandbox, and for
untrusted options, there is no default: only roots named explicitly (the
host's, for untrusted options: @"untrusted": {"panclude-roots": [...]}@).
A format's own includes inside an included file (LaTeX's @\\input@, RST's
@include@) read nothing: the file is read in pandoc's sandbox, with no
files.

A failure leaves the element as it was, with a warning in pandoc's log
(a 'ScriptingWarning', as the Lua filter's), which names the path, the
file holding the element and a fixed reason, never the file's content.
-}
module LibPandoc.Panclude
  ( Config (..)
  , Document (..)
  , document
  , panclude
  , entryPath
  , entryPrefix
  , filterEntries
  , hostRoots
  ) where

import Control.Exception (IOException, throwIO, try)
import Control.Monad (unless, when)
import Control.Monad.Except (ExceptT, catchError, runExceptT, throwError)
import Control.Monad.IO.Class (MonadIO, liftIO)
import Control.Monad.Trans.Class (lift)
import qualified Data.Aeson as Aeson
import qualified Data.Aeson.Key as K
import qualified Data.Aeson.KeyMap as KM
import qualified Data.ByteString as B
import qualified Data.ByteString.Lazy as BL
import Data.Char (isAsciiLower, isAsciiUpper, isDigit)
import Data.Data (Data, gmapM)
import Data.Foldable (toList)
import Data.List (isPrefixOf)
import Data.Maybe (fromMaybe, isJust, isNothing)
import Data.Monoid (Any (..))
import qualified Data.Set as Set
import qualified Data.Text as T
import qualified Data.Text.Encoding as TE
import Data.Typeable (cast)
import System.Directory (doesDirectoryExist, getSymbolicLinkTarget, makeAbsolute)
import System.FilePath (isAbsolute, joinPath, normalise, pathSeparator, splitDirectories,
                        takeDirectory, takeDrive, (</>))
import System.IO.Error (isDoesNotExistError)
import Text.Pandoc.Class (PandocMonad, report, sandbox)
import Text.Pandoc.Definition
import Text.Pandoc.Error (PandocError (..))
import Text.Pandoc.Format (formatFromFilePaths, formatName, parseFlavoredFormat)
import Text.Pandoc.Logging (LogMessage (ScriptingWarning))
import Text.Pandoc.Options (ReaderOptions (..))
import Text.Pandoc.Readers (Reader (..), getReader)
import qualified Text.Pandoc.UTF8 as UTF8
import Text.Pandoc.Walk (query, walk)

-- | What the step runs with.
data Config = Config
  { cfgRoots    :: Maybe [FilePath]
    -- ^ the directories it may read in, as given (relative ones from the
    -- working directory); Nothing: the main document's directory
  , cfgDocument :: Document
  , cfgFormat   :: T.Text
    -- ^ the main reader, with its extensions (@commonmark_x-smart@)
  , cfgReader   :: ReaderOptions
    -- ^ the main reader's options
  }

-- | The main document: where relative paths start.
data Document
  = File FilePath  -- ^ its first input file, as given
  | Stdin
  | URL FilePath   -- ^ read from a URL: nothing is included

-- | The main document of a conversion with these input files: the first
-- names it; none, @-@, or @stdinFile@ (the file holding the input given
-- to pandoc_convert) is standard input.
document :: Maybe FilePath -> Maybe [FilePath] -> Document
document stdinFile files = case files of
  Just (f : _)
    | f == "-" || Just f == stdinFile -> Stdin
    | isURL (T.pack f) -> URL f
    | otherwise -> File f
  _ -> Stdin

-- | The paths standing for the step among the filters, which libpandoc's
-- engine answers: 'entryPath' with the roots of an entry naming none
-- (@"panclude"@, @-F panclude@), 'entryPrefix' @i@ with the @i@th roots
-- 'filterEntries' found.
entryPath, entryPrefix :: FilePath
entryPath = "libpandoc:panclude"
entryPrefix = "libpandoc:panclude/"

-- | Each @{"type": "panclude", "roots": [...]}@ among the options'
-- @"filters"@ becomes the Lua filter 'entryPrefix' @i@, its roots the
-- @i@th of the list returned; one without @"roots"@ is 'entryPath', as the
-- bare @"panclude"@ (which pandoc's own parse leaves a JSON filter of that
-- name: libpandoc routes it). A field but @"type"@ and @"roots"@, or roots
-- that aren't a list of strings, is an error.
filterEntries :: Aeson.Value -> Either String ([[FilePath]], Aeson.Value)
filterEntries (Aeson.Object o)
  | Just (Aeson.Array fs) <- KM.lookup "filters" o = do
      entries <- traverse entry (toList fs)
      let (given, fs') = place 0 entries
      Right (given, Aeson.Object (KM.insert "filters" (Aeson.toJSON fs') o))
  where
    entry (Aeson.Object f)
      | KM.lookup "type" f == Just (Aeson.String "panclude") =
          case filter (`notElem` ["type", "roots"]) (KM.keys f) of
            k : _ -> Left $ "panclude: unknown field " <> show (K.toText k) <> " (its field: \"roots\")"
            [] -> case KM.lookup "roots" f of
              Nothing -> Right (Right Nothing)
              Just v -> Right . Just <$> directories "panclude: \"roots\"" v
    entry v = Right (Left v)
    -- (Left: another filter, as it is; Right: the step, with its roots)
    place :: Int -> [Either Aeson.Value (Maybe [FilePath])] -> ([[FilePath]], [Aeson.Value])
    place _ [] = ([], [])
    place i (Left v : es) = (v :) <$> place i es
    place i (Right Nothing : es) = (lua entryPath :) <$> place i es
    place i (Right (Just rs) : es) =
      let (g, r) = place (i + 1) es in (rs : g, lua (entryPrefix ++ show i) : r)
    lua p = Aeson.object [("type", Aeson.String "lua"), ("path", Aeson.toJSON p)]
filterEntries v = Right ([], v)

-- | The host's @"panclude-roots"@, which @"untrusted"@ adds to untrusted
-- options ("LibPandoc.Untrusted"): the roots of their @"panclude"@
-- entries, none if the host gave none; and the options without it.
-- Nothing for trusted options, for which it is no option (an error: their
-- entries carry their roots).
hostRoots :: Bool -> Aeson.Value -> Either String (Maybe [FilePath], Aeson.Value)
hostRoots isUntrusted (Aeson.Object o) = case KM.lookup "panclude-roots" o of
  Nothing -> Right (if isUntrusted then Just [] else Nothing, Aeson.Object o)
  Just v
    | isUntrusted -> do
        rs <- directories "\"untrusted\": \"panclude-roots\"" v
        Right (Just rs, Aeson.Object (KM.delete "panclude-roots" o))
    | otherwise -> Left
        "\"panclude-roots\" is the host's, inside \"untrusted\"; a trusted call names the roots in the filter: {\"type\": \"panclude\", \"roots\": [...]}"
hostRoots _ v = Right (Nothing, v)

-- | A list of directories' names.
directories :: String -> Aeson.Value -> Either String [FilePath]
directories what v = case Aeson.fromJSON v of
  Aeson.Success ds -> Right ds
  Aeson.Error _ -> Left $ what <> " is a list of directories, not " <> UTF8.toStringLazy (Aeson.encode v)

------------------------------------------------------------------------
-- The step

-- | Nested includes deeper than this fail.
maxDepth :: Int
maxDepth = 32

-- | Where the elements of one file are.
data Ctx = Ctx
  { ctxName  :: T.Text          -- ^ the file, in messages
  , ctxDir   :: FilePath        -- ^ its directory as written: the next file's name
  , ctxReal  :: Maybe FilePath  -- ^ its directory, absolute, links resolved (Nothing: a URL)
  , ctxDepth :: Int
  , ctxChain :: Set.Set FilePath -- ^ the files including it, links resolved
  }

data Env = Env
  { envRoots  :: [FilePath]     -- ^ absolute, links resolved
  , envFormat :: T.Text
  , envReader :: ReaderOptions
  }

-- | A file an element names.
data Located = Located
  { opened  :: FilePath  -- ^ as the Lua filter opens it: in messages
  , real    :: FilePath  -- ^ absolute, links resolved: what is read
  , realDir :: FilePath  -- ^ its directory as written, links resolved
  }

-- | The step: every include of the document done, in its metadata too.
-- The roots are checked first (one that isn't a directory is a
-- 'PandocOptionError', also when nothing is included).
panclude :: (PandocMonad m, MonadIO m) => Config -> Pandoc -> m Pandoc
panclude cfg doc = do
  ctx <- liftIO (mainContext (cfgDocument cfg))
  roots <- case cfgRoots cfg of
    Nothing -> pure (maybe [] pure (ctxReal ctx))
    Just rs -> mapM root rs
  let env = Env roots (cfgFormat cfg) (cfgReader cfg)
  if hasIncludes doc then expand env ctx doc else pure doc

-- | A root given: absolute, links resolved; one that isn't a directory is
-- an error.
root :: (PandocMonad m, MonadIO m) => FilePath -> m FilePath
root r = do
  resolved <- liftIO (try (resolve =<< makeAbsolute r) :: IO (Either IOException FilePath))
  isDir <- case resolved of
    Right d | not (null r) -> liftIO (doesDirectoryExist d)
    _ -> pure False
  case resolved of
    Right d | isDir -> pure d
    _ -> throwError $ PandocOptionError $
           "panclude: the root " <> T.pack (show r) <> " is not a directory"

mainContext :: Document -> IO Ctx
mainContext = \case
  Stdin -> do
    cwd <- resolve =<< makeAbsolute "."
    pure (Ctx "standard input" "." (Just cwd) 0 Set.empty)
  URL u -> pure (Ctx (T.pack u) "" Nothing 0 Set.empty)
  File f -> do
    dir <- resolve =<< makeAbsolute (takeDirectory f)
    self <- resolve =<< makeAbsolute f
    pure (Ctx (T.pack f) (takeDirectory f) (Just dir) 0 (Set.singleton self))

hasIncludes :: Pandoc -> Bool
hasIncludes = getAny . query (Any . isInclude)
  where
    isInclude = \case
      CodeBlock a _ -> includes a
      Div a _ -> includes a
      _ -> False
    includes (_, _, kvs) = isJust (lookup "include" kvs)

-- | Top down, so that a div's placeholder is never looked into: replaced
-- when its include succeeds, left as it was when it fails. (Generic, so
-- that every place a block can be is reached: notes, table cells, the
-- metadata.)
expand :: forall m a. (PandocMonad m, MonadIO m, Data a) => Env -> Ctx -> a -> m a
expand env ctx = go
  where
    go :: forall d. Data d => d -> m d
    go x
      | Just (b :: Block) <- cast x = case element b of
          Just act -> fromMaybe x . cast <$> act
          Nothing -> gmapM go x
      | Just (_ :: T.Text) <- cast x = pure x
      | otherwise = gmapM go x
    element = \case
      CodeBlock a@(_, _, kvs) t | Just p <- lookup "include" kvs -> Just (includeCode env ctx a t p)
      Div a@(_, _, kvs) bs | Just p <- lookup "include" kvs -> Just (includeDiv env ctx a bs p)
      _ -> Nothing

warn :: PandocMonad m => Ctx -> T.Text -> T.Text -> m ()
warn ctx path reason = report $ flip ScriptingWarning Nothing $
  "panclude: cannot include \"" <> path <> "\" in " <> ctxName ctx <> ": " <> reason

------------------------------------------------------------------------
-- A code block: the file's text

data CodeOptions = CodeOptions
  { startLine :: Maybe Integer
  , endLine   :: Maybe Integer
  , dedent    :: Maybe Integer
  }

includeCode :: (PandocMonad m, MonadIO m)
            => Env -> Ctx -> Attr -> T.Text -> T.Text -> m Block
includeCode env ctx attr@(ident, classes, kvs) placeholder path = do
  r <- runExceptT $ do
    opts <- either throwError pure (codeOptions kvs)
    file <- locate env ctx path
    text <- readText (real file)
    (,) opts <$> either throwError pure (selectLines opts text)
  case r of
    Left reason -> CodeBlock attr placeholder <$ warn ctx path reason
    Right (opts, text) -> pure $ case lookup "raw" kvs of
      Just f -> RawBlock (Format f) text
      Nothing -> CodeBlock (ident, classes, kept opts) text
  where
    kept opts =
      [kv | kv@(k, _) <- kvs, k `notElem` ["include", "startLine", "endLine", "start-line", "end-line", "dedent"]]
      ++ [("startFrom", tshow s) | Just s <- [startLine opts], isNothing (lookup "startFrom" kvs)]

-- | The attributes the step reads from a code block, checked: both
-- spellings of the range (the hyphenated one wins, as in
-- include-code-files), then dedent, then raw.
codeOptions :: [(T.Text, T.Text)] -> Either T.Text CodeOptions
codeOptions kvs = do
  s <- line "startLine" "start-line"
  e <- line "endLine" "end-line"
  d <- case lookup "dedent" kvs of
    Nothing -> Right Nothing
    Just t -> maybe (Left ("dedent=\"" <> t <> "\" is not a whole number")) (Right . Just) (whole False t)
  when (lookup "raw" kvs == Just "") $ Left "raw=\"\" names no format"
  Right (CodeOptions s e d)
  where
    line camel hyphen =
      let written = if isJust (lookup hyphen kvs) then hyphen else camel
      in case lookup written kvs of
        Nothing -> Right Nothing
        Just t -> case whole False t of
          Just n | n >= 1 -> Right (Just n)
          _ -> Left (written <> "=\"" <> t <> "\" is not a whole number from 1")

-- | The lines a code block's options select, dedented, joined by newlines.
selectLines :: CodeOptions -> T.Text -> Either T.Text T.Text
selectLines opts text
  | (isJust (startLine opts) || isJust (endLine opts)) && first > final =
      Left ("the range selects none of the file's " <> tshow n <> " lines")
  | otherwise = Right $ T.intercalate "\n" $ map undent $
      take (fromInteger (final - first + 1)) (drop (fromInteger (first - 1)) ls)
  where
    -- (the final newline ends the last line: an empty file has none)
    ls = T.lines text
    n = toInteger (length ls)
    first = fromMaybe 1 (startLine opts)
    final = min n (fromMaybe n (endLine opts))
    undent l = case dedent opts of
      Nothing -> l
      Just d -> T.drop (fromInteger (min d (toInteger (T.length (T.takeWhile (== ' ') l))))) l

-- | A whole number in decimal digits, that fits in 64 bits (as Lua's
-- integers); a minus sign only if @negative@.
whole :: Bool -> T.Text -> Maybe Integer
whole negative t = case T.uncons t of
  Just ('-', ds) | negative -> fits . negate =<< digits ds
  _ -> fits =<< digits t
  where
    digits ds
      | not (T.null ds) && T.all isDigit ds = Just (read (T.unpack ds))
      | otherwise = Nothing
    fits x
      | x >= -9223372036854775808 && x <= 9223372036854775807 = Just x
      | otherwise = Nothing

------------------------------------------------------------------------
-- A div: the file as a document

includeDiv :: (PandocMonad m, MonadIO m)
           => Env -> Ctx -> Attr -> [Block] -> T.Text -> m Block
includeDiv env ctx attr@(ident, classes, kvs) placeholder path = do
  r <- runExceptT $ do
    shift <- case lookup "shift-heading-level-by" kvs of
      Nothing -> pure 0
      Just t -> maybe (throwError ("shift-heading-level-by=\"" <> t <> "\" is not a whole number"))
                      pure (whole True t)
    file <- locate env ctx path
    when (real file `Set.member` ctxChain ctx) $
      throwError "a cycle: the file is already being included"
    text <- readText (real file)
    let fmt = readerFormat env (lookup "format" kvs) path
    blocks <- lift (readDocument (envReader env) fmt text)
    maybe (throwError ("pandoc cannot read it as \"" <> fmt <> "\"")) (pure . (,,) file shift) blocks
  case r of
    Left reason -> Div attr placeholder <$ warn ctx path reason
    Right (file, shift, blocks) -> do
      let inner = Ctx (T.pack (opened file)) (takeDirectory (opened file)) (Just (realDir file))
                      (ctxDepth ctx + 1) (Set.insert (real file) (ctxChain ctx))
      blocks' <- expand env inner blocks
      pure $ Div (ident, classes, [kv | kv@(k, _) <- kvs, k `notElem` ["include", "format", "shift-heading-level-by"]])
                 (rebase (directoryOf path) (shiftHeadings shift blocks'))

-- | The format to read a div's file with: the div's own; else the one the
-- file's extension names, if not markdown; else the main document's.
readerFormat :: Env -> Maybe T.Text -> T.Text -> T.Text
readerFormat env given path = case given of
  Just f -> f
  Nothing -> case formatName <$> formatFromFilePaths [T.unpack path] of
    Just name | name /= "markdown" -> name
    _ -> envFormat env

-- | A file's blocks, read as Lua's pandoc.read reads a text (the format's
-- own extensions, the main reader's other options, but not standalone),
-- but in pandoc's sandbox with no files: a format's own includes (LaTeX's
-- @\\input@, RST's @include@) read nothing, as they would read outside
-- the roots. Nothing if it can't be read (pandoc's message, which can
-- quote the file, is not passed on).
readDocument :: (PandocMonad m, MonadIO m) => ReaderOptions -> T.Text -> T.Text -> m (Maybe [Block])
readDocument ropts fmt text = (Just <$> sandbox [] reading) `catchError` \_ -> pure Nothing
  where
    reading = do
      (reader, exts) <- getReader =<< parseFlavoredFormat fmt
      let o = ropts { readerExtensions = exts, readerStandalone = False }
      Pandoc _ blocks <- case reader of
        TextReader r -> r o text'
        ByteStringReader r -> r o (BL.fromStrict (TE.encodeUtf8 text'))
      pure blocks
    -- (as pandoc.read's UTF8.toText: a second byte order mark goes too)
    text' = fromMaybe text (T.stripPrefix "\xFEFF" text)

shiftHeadings :: Integer -> [Block] -> [Block]
shiftHeadings 0 = id
shiftHeadings by = walk $ \case
  Header l a c
    | toInteger l + by < 1 -> Para c
    | otherwise -> Header (fromInteger (toInteger l + by)) a c
  b -> b

-- | The included content's relative paths, rewritten to name the same
-- files from the including file's directory: an image's, a link's, and
-- the include of an element whose include failed.
rebase :: T.Text -> [Block] -> [Block]
rebase "" = id
rebase prefix = walk inline . walk block
  where
    inline = \case
      Image a c (src, t) -> Image a c (rebaseTarget prefix src, t)
      Link a c (src, t) -> Link a c (rebaseTarget prefix src, t)
      x -> x
    block = \case
      CodeBlock a t -> CodeBlock (rebaseInclude a) t
      Div a bs -> Div (rebaseInclude a) bs
      x -> x
    rebaseInclude (i, c, kvs) =
      (i, c, [(k, if k == "include" then rebaseTarget prefix v else v) | (k, v) <- kvs])

rebaseTarget :: T.Text -> T.Text -> T.Text
rebaseTarget prefix target
  | relativeTarget target = prefix <> sep <> fromMaybe target (T.stripPrefix "./" target)
  | otherwise = target
  where sep = if "/" `T.isSuffixOf` prefix then "" else "/"

-- | Not empty, not a fragment or query, not absolute, and no URL (a
-- Windows drive included).
relativeTarget :: T.Text -> Bool
relativeTarget t = case T.uncons t of
  Nothing -> False
  Just (c, rest) -> c `notElem` ("#?/\\" :: String)
    && not (asciiLetter c && ":" `T.isPrefixOf` T.dropWhile schemeChar rest)

-- | The directory part of a path, as written, with @/@ between its parts
-- (on Windows @\\@ counts as @/@), a leading @./@ dropped; "" when it has
-- none.
directoryOf :: T.Text -> T.Text
directoryOf p = case T.breakOnEnd "/" p' of
  ("", _) -> ""
  (pre, _) -> let dir = T.dropEnd 1 pre in if T.null dir then "/" else clean dir
  where
    p' = if pathSeparator == '\\' then T.replace "\\" "/" p else p
    clean d = case T.stripPrefix "./" d of
      Just rest -> clean rest
      Nothing -> if d == "." then "" else d

------------------------------------------------------------------------
-- Paths and files

-- | The file an element names, checked: its path (empty, a URL, in a
-- document read from a URL), the depth, then the roots: the path as
-- written must lie inside one, and so must the file once symbolic links
-- are resolved. The reason when it names none it may read.
locate :: MonadIO m => Env -> Ctx -> T.Text -> ExceptT T.Text m Located
locate env ctx path = do
  when (T.null path) $ throwError "the path is empty"
  when (isURL path) $ throwError "a URL is not a path"
  base <- maybe (throwError "the document is a URL, so no file is included") pure (ctxReal ctx)
  when (ctxDepth ctx >= maxDepth) $ throwError ("more than " <> tshow maxDepth <> " nested includes")
  when (null (envRoots env)) $ throwError "there are no roots, so no file is included"
  let p = T.unpack path
      -- (</> keeps p when it is absolute)
      written = base </> p
  unless (inside (lexical written)) $ throwError "outside the roots"
  file <- resolved written
  unless (inside file && inside (lexical file)) $
    throwError "a symbolic link leads outside the roots"
  dir <- resolved (takeDirectory written)
  let name | isAbsolute p || ctxDir ctx `elem` [".", ""] = p
           | otherwise = ctxDir ctx </> p
  pure (Located name file dir)
  where
    inside q = any (`within` q) (envRoots env)
    resolved q = liftIO (try (resolve q)) >>= \case
      Left (_ :: IOException) -> throwError "cannot be read"
      Right r -> pure r

-- | A file's text: UTF-8 checked, a byte order mark dropped, line endings
-- as @\\n@. The reason when it can't be had: a few fixed phrases, never
-- the system's message.
readText :: MonadIO m => FilePath -> ExceptT T.Text m T.Text
readText f = do
  r <- liftIO (try (B.readFile f))
  bytes <- case r of
    Left e | isDoesNotExistError e -> throwError "not found"
           | otherwise -> throwError "cannot be read"
    Right b -> pure b
  text <- either (const (throwError "not UTF-8")) pure (TE.decodeUtf8' bytes)
  pure $ T.replace "\r" "\n" $ T.replace "\r\n" "\n" $ fromMaybe text (T.stripPrefix "\xFEFF" text)

-- | An absolute path with its symbolic links resolved, as realpath does,
-- part by part (so that it works where the system's realpath can't see
-- the whole path: WASI, which opens only some directories). A part that
-- isn't a link, or can't be read as one (it doesn't exist), is kept as it
-- is; @..@ after a part without links is its parent. More than 40 links
-- is an error.
resolve :: FilePath -> IO FilePath
resolve = go (40 :: Int) "" . parts
  where
    go _ done [] = pure done
    go n "" (c : cs) = go n c cs -- the root
    go n done (c : cs)
      | c == "." = go n done cs
      | c == ".." = go n (takeDirectory done) cs
      | otherwise = do
          let p = done </> c
          target <- try (getSymbolicLinkTarget p)
          case target of
            Left (_ :: IOException) -> go n p cs
            Right t
              | n <= 0 -> throwIO (userError "too many symbolic links")
              | otherwise -> go (n - 1) "" (parts (done </> t) ++ cs)

-- | A path with @.@ and @..@ resolved by its text alone (@..@ at the top
-- stays there).
lexical :: FilePath -> FilePath
lexical = joinPath . reverse . foldl step [] . parts
  where
    step acc "." = acc
    step acc@(a : rest) ".."
      | a == takeDrive a = acc -- the root
      | a /= ".." = rest
    step acc part = part : acc

-- | Whether a path lies in a directory (both absolute, links resolved).
within :: FilePath -> FilePath -> Bool
within dir p = parts dir `isPrefixOf` parts p

-- | A path's parts, its root (@/@, @C:\\@) first, spelled one way (on
-- Windows @C:/@ is @C:\\@).
parts :: FilePath -> [FilePath]
parts = splitDirectories . normalise

-- | A URL: a scheme of two letters or more, then a colon. One letter and a
-- colon is a Windows drive.
isURL :: T.Text -> Bool
isURL t = case T.uncons t of
  Just (c, rest) | asciiLetter c ->
    let (scheme, after) = T.span schemeChar rest
    in not (T.null scheme) && ":" `T.isPrefixOf` after
  _ -> False

asciiLetter, schemeChar :: Char -> Bool
asciiLetter c = isAsciiLower c || isAsciiUpper c
schemeChar c = asciiLetter c || isDigit c || c `elem` ("+.-" :: String)

tshow :: Show a => a -> T.Text
tshow = T.pack . show
