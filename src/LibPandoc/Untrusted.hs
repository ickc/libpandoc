{-# LANGUAGE OverloadedStrings #-}
{- |
   Module      : LibPandoc.Untrusted
   Copyright   : Copyright (C) 2026 Kolen Cheung
   License     : GNU GPL, version 2 or above

@"untrusted": true@: options (and queries) from code the host doesn't
trust, such as a wasm filter calling pandoc. Only what reads no files,
writes none, fetches nothing and runs no programs is accepted, and pandoc's
sandbox is on. The one list every host shares (pandocrs, libpandoc.wasm's
JavaScript, LibPandoc.jl), rather than a copy in each.
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
-- writers), and not @pdf@ (which runs a PDF engine).
writeOptions :: [T.Text]
writeOptions =
  [ "to", "writer", "ascii", "cite-method", "dpi", "email-obfuscation", "eol"
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

-- | The options without the key @untrusted@; if it was @true@, checked
-- (Left names what isn't allowed) and with @"sandbox": true@.
untrusted :: Call -> Aeson.Value -> Either String Aeson.Value
untrusted call (Aeson.Object o) = case KM.lookup "untrusted" o of
  Nothing -> Right (Aeson.Object o)
  Just (Aeson.Bool False) -> Right (Aeson.Object o')
  Just (Aeson.Bool True) -> do
    mapM_ check (KM.toList o')
    Right (Aeson.Object (KM.insert "sandbox" (Aeson.Bool True) o'))
  Just v -> Left ("\"untrusted\" is true or false, not " <> show v)
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
untrusted _ v = Right v  -- not options: pandoc's parse says so

-- | A query with @"untrusted": true@ is one of 'queries' (Left otherwise).
untrustedQuery :: Aeson.Object -> Either String ()
untrustedQuery o = case KM.lookup "untrusted" o of
  Just (Aeson.Bool True) -> case KM.lookup "query" o of
    Just (Aeson.String q) | q `elem` queries -> Right ()
    q -> Left ("query not allowed for untrusted code: " <> maybe "none" show q)
  _ -> Right ()

refuse :: String -> Either String a
refuse what = Left ("not allowed for untrusted code: " <> what)

-- | A format's name with extensions (@commonmark_x+smart-raw_html@), not a
-- path to a Lua reader or writer.
formatName :: T.Text -> Bool
formatName f = not (T.null f) && all part (T.split (`elem` ['+', '-']) f)
  where
    part p = not (T.null p) && T.all word p
    word c = isAsciiLower c || isAsciiUpper c || isDigit c || c == '_'
