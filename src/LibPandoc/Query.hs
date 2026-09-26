{-# LANGUAGE OverloadedStrings   #-}
{-# LANGUAGE RankNTypes          #-}
{-# LANGUAGE ScopedTypeVariables #-}
{-# LANGUAGE TemplateHaskell     #-}
{- |
   Module      : LibPandoc.Query
   Copyright   : Copyright (C) 2006-2024 John MacFarlane, 2026 Kolen Cheung
   License     : GNU GPL, version 2 or above

Answers @pandoc_query@. The queries and their answers are those of
upstream's pandoc.wasm (@pandoc-cli/wasm/PandocWasm.hs@), from which most of
this is taken, plus @api-version@.
-}
module LibPandoc.Query (query) where

import qualified Control.Exception as E
import Data.Aeson (FromJSON (..), ToJSON, withObject, (.:))
import qualified Data.Aeson as Aeson
import qualified Data.ByteString as B
import qualified Data.ByteString.Lazy as BL
import Data.List (sort)
import qualified Data.Map as M
import qualified Data.Text as T
import Data.Version (showVersion, versionBranch)
import Skylighting (Syntax (..), defaultSyntaxMap)
import System.FilePath (splitExtension)
import Text.Pandoc (PandocIO, Reader, Writer, pandocVersion, readers, runIO,
                    setUserDataDir, writers)
import Text.Pandoc.Definition (pandocTypesVersion)
import Text.Pandoc.Error (PandocError (..))
import Text.Pandoc.Extensions (extensionEnabled, extensionsToList,
                               getAllExtensions, getDefaultExtensions)
import Text.Pandoc.Highlighting (highlightingStyles)
import Text.Pandoc.Lua (getEngine)
import Text.Pandoc.Scripting (ScriptingEngine (..), customTemplate)
import Text.Pandoc.Templates (getDefaultTemplate)

data Query
  = PandocVersion
  | ApiVersion
  | InputFormats
  | OutputFormats
  | HighlightLanguages
  | HighlightStyles
  | ExtensionsForFormat T.Text
  | DefaultTemplate T.Text

instance FromJSON Query where
  parseJSON = withObject "Query" $ \o -> do
    queryType <- o .: "query"
    case queryType of
      "version" -> pure PandocVersion
      "api-version" -> pure ApiVersion
      "input-formats" -> pure InputFormats
      "output-formats" -> pure OutputFormats
      "highlight-languages" -> pure HighlightLanguages
      "highlight-styles" -> pure HighlightStyles
      "default-template" -> DefaultTemplate <$> o .: "format"
      "extensions-for-format" -> ExtensionsForFormat <$> o .: "format"
      _ -> fail $ "Unknown query type " <> queryType

-- | Answer a JSON-encoded query with JSON.
query :: B.ByteString -> IO B.ByteString
query json =
  case Aeson.eitherDecodeStrict json of
    Left e -> E.throwIO $ PandocOptionError $ T.pack e
    Right q -> answer q

answer :: Query -> IO B.ByteString
answer q = case q of
  PandocVersion -> jsonOut $ showVersion pandocVersion
  ApiVersion -> jsonOut $ versionBranch pandocTypesVersion
  HighlightStyles -> jsonOut $ map fst highlightingStyles
  HighlightLanguages -> jsonOut $ sort
    [ T.toLower (sShortname s)
    | s <- M.elems defaultSyntaxMap
    , sShortname s `notElem` ["Alert", "Alert_indent"]
    ]
  DefaultTemplate format -> do
    templ <- runIO $
      case splitExtension (T.unpack format) of
        (_, "") -> do
          -- built-in format
          setUserDataDir Nothing
          getDefaultTemplate format
        _ -> do
          -- format looks like a filepath => custom writer
          engine <- getEngine
          components <- engineLoadCustom engine (T.unpack format)
          case customTemplate components of
            Just t  -> pure t
            Nothing -> E.throw $ PandocNoTemplateError format
    case templ of
      Right t
        | T.null t -> -- e.g. for docx, odt, json:
            E.throwIO $ PandocCouldNotFindDataFileError $
              "templates/default." <> format
        | otherwise -> jsonOut t
      Left e -> E.throwIO e
  InputFormats -> jsonOut $ sort (map fst (readers :: [(T.Text, Reader PandocIO)]))
  OutputFormats -> jsonOut $ sort
    ("pdf" : map fst (writers :: [(T.Text, Writer PandocIO)]))
  ExtensionsForFormat format -> do
    let allExts = getAllExtensions format
        defExts = getDefaultExtensions format
        addExt x = M.insert (drop 4 (show x)) (extensionEnabled x defExts)
    jsonOut $ foldr addExt mempty (extensionsToList allExts)
 where
  jsonOut :: forall a. ToJSON a => a -> IO B.ByteString
  jsonOut = pure . BL.toStrict . Aeson.encode
