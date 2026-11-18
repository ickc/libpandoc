{-# LANGUAGE LambdaCase        #-}
{-# LANGUAGE OverloadedStrings #-}
{- |
   Module      : LibPandoc.Untrusted
   Copyright   : Copyright (C) 2026 Kolen Cheung
   License     : GNU GPL, version 2 or above

@"untrusted": true@: options (and queries) from code the host doesn't
trust, such as a wasm filter calling pandoc. Only what reads no files,
writes none, fetches nothing and runs no programs is accepted, and pandoc's
sandbox is on. That includes citeproc, which libpandoc runs inside the
sandbox too, with the files the options name; untrusted options name none
(no @bibliography@, @csl@, @citation-abbreviations@), so only a document's
own references, in its metadata, work. The one list every host shares
(pandocrs, libpandoc.wasm's JavaScript, LibPandoc.jl), rather than a copy
in each.

The host sets @"untrusted"@ itself, over whatever the code wrote, so its
value is the host's: @true@, or an object of what the host adds to the
code's options (its @"cancel-token"@, which the code's options can't name).
-}
module LibPandoc.Untrusted
  ( Call (..)
  , untrusted
  , untrustedQuery
  , readOptions
  , writeOptions
  , queries
  ) where

import qualified Data.Aeson as Aeson
import qualified Data.Aeson.Key as K
import qualified Data.Aeson.KeyMap as KM
import Data.Char (isAsciiLower, isAsciiUpper, isDigit)
import qualified Data.Text as T

-- | Which call the options are for.
data Call = Convert | ReadMany
  deriving (Eq, Show)

-- | Options that affect reading: for both calls. Formats are names (not
-- Lua readers).
readOptions :: [T.Text]
readOptions =
  [ "from", "reader", "columns", "default-image-extension"
  , "indented-code-classes", "preserve-tabs", "strip-comments", "tab-stop"
  , "track-changes", "sandbox" ]

-- | With 'readOptions', for 'Convert'. Formats are names (not Lua
-- writers), and not @pdf@ (which runs a PDF engine). @citeproc@: in the
-- sandbox (LibPandoc's @citeprocSandboxed@).
writeOptions :: [T.Text]
writeOptions =
  [ "to", "writer", "ascii", "citeproc", "cite-method", "dpi"
  , "email-obfuscation", "eol"
  , "fail-if-warnings", "figure-caption-position", "html-math-method"
  , "html-q-tags", "identifier-prefix", "incremental", "list-tables"
  , "listings", "markdown-headings", "metadata", "number-offset"
  , "number-sections", "reference-links", "reference-location"
  , "reference-section-title", "section-divs", "shift-heading-level-by"
  , "slide-level", "split-level", "standalone", "table-caption-position"
  , "table-of-contents", "title-prefix", "toc", "toc-depth"
  , "top-level-division", "variables", "verbosity", "wrap" ]

-- | The queries untrusted code may make: not @parse-args@, which reads
-- defaults files, nor @default-template@, which reads the user's.
queries :: [T.Text]
queries =
  [ "version", "api-version", "input-formats", "output-formats"
  , "highlight-languages", "highlight-styles", "extensions-for-format"
  , "num-threads" ]

-- | What @"untrusted"@ says ('trust' reads it), and the options without
-- it; if untrusted, checked (Left names what isn't allowed), with
-- @"sandbox": true@ and with what the host adds (its @"cancel-token"@,
-- which the check has refused among the code's options).
untrusted :: Call -> Aeson.Value -> Either String (Bool, Aeson.Value)
untrusted call (Aeson.Object o) = trust o >>= \case
  Nothing -> Right (False, Aeson.Object o')
  Just host -> do
    mapM_ check (KM.toList o')
    Right (True, Aeson.Object (KM.union host (KM.insert "sandbox" (Aeson.Bool True) o')))
  where
    o' = KM.delete "untrusted" o
    allowed k = k `elem` readOptions || (call == Convert && k `elem` writeOptions)
    check (key, v)
      | not (allowed k) = refuse (T.unpack k)
      | k `elem` ["from", "reader", "to", "writer"] = case v of
          Aeson.String f | formatName f
                         , not (f == "pdf" && k `elem` ["to", "writer"]) -> Right ()
          _ -> refuse (T.unpack k <> ": " <> show v)
      | otherwise = Right ()
      where k = K.toText key
untrusted _ v = Right (False, v)  -- not options: pandoc's parse says so

-- | An untrusted query (any @"untrusted"@ but absent or @false@) is one of
-- 'queries', and takes no cancel token (Left otherwise).
untrustedQuery :: Aeson.Object -> Either String ()
untrustedQuery o = trust o >>= \case
  Nothing -> Right ()
  Just host
    | KM.member "cancel-token" host -> Left "\"untrusted\": a query takes no \"cancel-token\""
    | otherwise -> case KM.lookup "query" o of
        Just (Aeson.String q) | q `elem` queries -> Right ()
        q -> Left ("query not allowed for untrusted code: " <> maybe "none" show q)

-- | The value of @"untrusted"@, the one reading of it for every call:
-- trusted (Nothing) only when it is absent or @false@; untrusted when it is
-- @true@ (the host adds nothing) or an object of what the host adds (only
-- @"cancel-token"@); anything else is an error, so that nothing is taken
-- for trusted by mistake.
trust :: Aeson.Object -> Either String (Maybe Aeson.Object)
trust o = case KM.lookup "untrusted" o of
  Nothing -> Right Nothing
  Just (Aeson.Bool False) -> Right Nothing
  Just (Aeson.Bool True) -> Right (Just KM.empty)
  Just (Aeson.Object host) -> case filter (/= "cancel-token") (KM.keys host) of
    [] -> Right (Just host)
    k : _ -> Left ("\"untrusted\": unknown field " <> show (K.toText k)
                   <> " (the host's own options: \"cancel-token\")")
  Just v -> Left ("\"untrusted\" is true, false or an object, not " <> show v)

refuse :: String -> Either String a
refuse what = Left ("not allowed for untrusted code: " <> what)

-- | A format's name with extensions (@commonmark_x+smart-raw_html@), not a
-- path to a Lua reader or writer.
formatName :: T.Text -> Bool
formatName f = not (T.null f) && all part (T.split (`elem` ['+', '-']) f)
  where
    part p = not (T.null p) && T.all word p
    word c = isAsciiLower c || isAsciiUpper c || isDigit c || c == '_'
