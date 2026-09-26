{-# LANGUAGE TemplateHaskell #-}
{- |
   Module      : LibPandoc.Schema
   Copyright   : Copyright (C) 2026 Kolen Cheung
   License     : GNU GPL, version 2 or above

The schema of pandoc's document AST, generated at compile time from the
pandoc-types this library is built with. See "LibPandoc.SchemaTH".
-}
module LibPandoc.Schema (astSchema) where

import qualified Data.ByteString as B
import qualified Text.Pandoc.UTF8 as UTF8
import Text.Pandoc.Definition (Pandoc)

import LibPandoc.SchemaTH (schemaOf)

-- | JSON: @{"root": "Pandoc", "types": [...]}@.
astSchema :: B.ByteString
astSchema = UTF8.fromString $(schemaOf ''Pandoc)
