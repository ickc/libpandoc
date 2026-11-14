{-# LANGUAGE FlexibleInstances #-}
{-# LANGUAGE FlexibleContexts  #-}
{-# LANGUAGE OverloadedStrings #-}
{-# LANGUAGE TypeOperators     #-}
{- |
   Module      : LibPandoc.Options
   Copyright   : Copyright (C) 2026 Kolen Cheung
   License     : GNU GPL, version 2 or above

pandoc's own JSON of a whole 'Opt' (its 'ToJSON' and 'FromJSON' instances),
as @parse-args@ returns it and @pandoc_convert@ takes it back under
@"opt"@: so that a host can handle a command line through the library
path (parse it, change what it names, convert), with the conversion pandoc
would run for those arguments.

pandoc's two instances are each other's inverse; 'toOptJSON' checks it for
the options at hand, and 'fromOptJSON' takes only the keys 'Opt' has, so
that a pandoc whose 'Opt' changes shows up in the tests, not as options
silently lost.
-}
module LibPandoc.Options
  ( toOptJSON
  , fromOptJSON
  , optKeys
  ) where

import qualified Data.Aeson as Aeson
import qualified Data.Aeson.Key as K
import qualified Data.Aeson.KeyMap as KM
import Data.Aeson.Types (camelTo2)
import qualified Data.Text as T
import GHC.Generics
import Text.Pandoc.App (Opt, defaultOpts)

-- | pandoc's JSON of these options, if it gives them back exactly (always
-- so far: 'Nothing' would be a pandoc whose two instances disagree).
toOptJSON :: Opt -> Maybe Aeson.Value
toOptJSON o = case Aeson.fromJSON v of
  Aeson.Success o' | show (o' :: Opt) == show o -> Just v
  _ -> Nothing
  where v = Aeson.toJSON o

-- | Options from pandoc's JSON of them; a key that isn't one of 'Opt''s
-- fields is an error (pandoc's own instance would ignore it). Missing
-- keys are pandoc's defaults.
fromOptJSON :: Aeson.Value -> Either String Opt
fromOptJSON (Aeson.Object o)
  | unknown@(_ : _) <- filter (`notElem` optKeys) (map K.toText (KM.keys o)) =
      Left $ "opt: not an option of pandoc's: " <> T.unpack (T.intercalate ", " unknown)
  | otherwise = case Aeson.fromJSON (Aeson.Object o) of
      Aeson.Success opts -> Right opts
      Aeson.Error e -> Left ("opt: " <> e)
fromOptJSON _ = Left "opt: an object (pandoc's JSON of its options, as parse-args gives it)"

-- | The keys of pandoc's JSON of 'Opt': its fields, named as its 'ToJSON'
-- instance names them.
optKeys :: [T.Text]
optKeys = map (T.pack . camelTo2 '-' . drop 3) (fields (from defaultOpts))

-- | A record's field names, from its 'Generic' representation.
class Fields f where
  fields :: f p -> [String]
instance Fields f => Fields (D1 c f) where
  fields (M1 x) = fields x
instance Fields f => Fields (C1 c f) where
  fields (M1 x) = fields x
instance (Fields f, Fields g) => Fields (f :*: g) where
  fields (x :*: y) = fields x ++ fields y
instance Selector s => Fields (S1 s f) where
  fields m = [selName m]
