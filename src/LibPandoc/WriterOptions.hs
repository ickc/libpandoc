{-# LANGUAGE OverloadedStrings #-}
{-# LANGUAGE RecordWildCards   #-}
{- |
   Module      : LibPandoc.WriterOptions
   Copyright   : Copyright (C) 2026 Kolen Cheung
   License     : GNU GPL, version 2 or above

The writer's options as JSON, for the filters libpandoc runs: JSON filters
in @PANDOC_WRITER_OPTIONS@, callback filters in their context's
@"writer-options"@, as both get the reader's (@PANDOC_READER_OPTIONS@,
@"reader-options"@). pandoc has no JSON encoder for 'WriterOptions'; it
gives them to Lua filters only (@PANDOC_WRITER_OPTIONS@), and this follows
that: the same fields with the same values, keyed as pandoc's JSON keys
'ReaderOptions' (kebab-case of the field's name without @writer@, as an
upstream @deriveJSON@ like the reader's would: @number-sections@,
@toc-depth@, @math-method@ where Lua says @html_math_method@).

* @template@: @true@ if there is one (standalone output), else @null@
  (Lua gives an opaque Template object, not its text);
* @variables@: a map of text, lists and maps (doctemplates' JSON; Lua
  gives the same, as Doc objects);
* not @syntax-map@ (the syntax definitions; nor does Lua), nor Lua's
  @epub_chapter_level@ (a deprecated synonym of @split_level@).
-}
module LibPandoc.WriterOptions
  ( writerOptionsJSON
  , writerOptionsVariable
  ) where

import Data.Aeson (Value (Bool, Null, Object), encode, object, (.=))
import qualified Data.Aeson.KeyMap as KM
import qualified Data.ByteString.Lazy as BL
import Text.Pandoc.Options (WriterOptions (..))

-- | The writer's options, as described above.
writerOptionsJSON :: WriterOptions -> Value
writerOptionsJSON WriterOptions{..} = object
  [ "template"                .= maybe Null (const (Bool True)) writerTemplate
  , "variables"               .= writerVariables
  , "tab-stop"                .= writerTabStop
  , "table-of-contents"       .= writerTableOfContents
  , "list-of-figures"         .= writerListOfFigures
  , "list-of-tables"          .= writerListOfTables
  , "incremental"             .= writerIncremental
  , "math-method"             .= writerMathMethod
  , "number-sections"         .= writerNumberSections
  , "number-offset"           .= writerNumberOffset
  , "section-divs"            .= writerSectionDivs
  , "extensions"              .= writerExtensions
  , "reference-links"         .= writerReferenceLinks
  , "dpi"                     .= writerDpi
  , "wrap-text"               .= writerWrapText
  , "columns"                 .= writerColumns
  , "email-obfuscation"       .= writerEmailObfuscation
  , "identifier-prefix"       .= writerIdentifierPrefix
  , "cite-method"             .= writerCiteMethod
  , "html-q-tags"             .= writerHtmlQTags
  , "slide-level"             .= writerSlideLevel
  , "top-level-division"      .= writerTopLevelDivision
  , "highlight-method"        .= writerHighlightMethod
  , "setext-headers"          .= writerSetextHeaders
  , "list-tables"             .= writerListTables
  , "epub-subdirectory"       .= writerEpubSubdirectory
  , "epub-metadata"           .= writerEpubMetadata
  , "epub-fonts"              .= writerEpubFonts
  , "epub-title-page"         .= writerEpubTitlePage
  , "split-level"             .= writerSplitLevel
  , "chunk-template"          .= writerChunkTemplate
  , "toc-depth"               .= writerTOCDepth
  , "reference-doc"           .= writerReferenceDoc
  , "reference-location"      .= writerReferenceLocation
  , "figure-caption-position" .= writerFigureCaptionPosition
  , "table-caption-position"  .= writerTableCaptionPosition
  , "prefer-ascii"            .= writerPreferAscii
  , "link-images"             .= writerLinkImages
  ]

-- | 'writerOptionsJSON', encoded for an environment variable: without
-- @variables@ when the whole is longer than 32,000 bytes (they may hold
-- whole files, @--include-in-header@'s), as a variable's value is at most
-- 32,767 characters on Windows and 128 KiB on Linux, and a filter whose
-- environment is too large wouldn't start.
writerOptionsVariable :: WriterOptions -> BL.ByteString
writerOptionsVariable w
  | BL.length full <= 32000 = full
  | Object o <- json = encode (Object (KM.delete "variables" o))
  | otherwise = full
  where
    json = writerOptionsJSON w
    full = encode json

-- | Every field of 'WriterOptions', by position (39 in pandoc 3.14): this
-- stops compiling when pandoc adds a field or removes one, so that
-- 'writerOptionsJSON' is looked at again at a pandoc bump (a field
-- renamed or removed stops it compiling too). Not encoded:
-- @writerSyntaxMap@.
_everyField :: WriterOptions -> ()
_everyField (WriterOptions _ _ _ _ _ _ _ _ _ _ _ _ _ _ _ _ _ _ _ _
                           _ _ _ _ _ _ _ _ _ _ _ _ _ _ _ _ _ _ _) = ()
