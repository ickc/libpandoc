{-# LANGUAGE OverloadedStrings #-}
{-# LANGUAGE TemplateHaskell   #-}
{- |
   Module      : LibPandoc.SchemaTH
   Copyright   : Copyright (C) 2026 Kolen Cheung
   License     : GNU GPL, version 2 or above

Describes pandoc's document types as JSON, at compile time, by reifying the
declarations in pandoc-types. Bindings generate their AST classes from this
description, so a pandoc-types change reaches them without anyone copying
type definitions by hand.

Every type reachable from the root that is declared in
"Text.Pandoc.Definition" is described. Anything else must be one of the
primitives below; an unknown type stops the build, so that a change in
pandoc-types can't silently produce a wrong schema.
-}
module LibPandoc.SchemaTH (schemaOf) where

import Data.Aeson (Value, object, (.=))
import qualified Data.Aeson as Aeson
import qualified Data.ByteString.Lazy.Char8 as BL8
import qualified Data.Aeson.Key as Key
import Data.List (nub)
import Data.Version (versionBranch)
import Language.Haskell.TH
import Text.Pandoc.Definition (pandocTypesVersion)

definitionModule :: String
definitionModule = "Text.Pandoc.Definition"

-- | A string literal holding the JSON schema of every type reachable from
-- the given one.
schemaOf :: Name -> Q Exp
schemaOf root = do
  decls <- collect [root] []
  let schema = object
        [ "pandoc-api-version" .= versionBranch pandocTypesVersion
        , "root" .= nameBase root
        , "types" .= map snd decls
        ]
  litE (stringL (BL8.unpack (Aeson.encode schema)))

-- | Breadth-first over referenced types, in order of first reference.
collect :: [Name] -> [(Name, Value)] -> Q [(Name, Value)]
collect [] done = pure (reverse done)
collect (n : todo) done
  | n `elem` map fst done = collect todo done
  | otherwise = do
      (val, refs) <- describeDecl n
      collect (todo ++ nub refs) ((n, val) : done)

describeDecl :: Name -> Q (Value, [Name])
describeDecl n = do
  info <- reify n
  case info of
    TyConI (DataD [] _ [] _ cons _) -> do
      (cs, refs) <- unzip <$> mapM describeCon cons
      pure (decl "data" ["constructors" .= cs], concat refs)
    TyConI (NewtypeD [] _ [] _ con _) -> do
      (c, refs) <- describeCon con
      pure (decl "newtype" ["constructors" .= [c]], refs)
    TyConI (TySynD _ [] ty) -> do
      (t, refs) <- describeType ty
      pure (decl "alias" ["type" .= t], refs)
    _ -> fail $ "LibPandoc.SchemaTH: unsupported declaration of " ++ show n
 where
  decl :: String -> [(Key.Key, Value)] -> Value
  decl kind rest = object (["name" .= nameBase n, "kind" .= kind] ++ rest)

describeCon :: Con -> Q (Value, [Name])
describeCon con = case con of
  NormalC n fields -> build n [(Nothing :: Maybe String, t) | (_, t) <- fields]
  RecC n fields -> build n [(Just (nameBase f), t) | (f, _, t) <- fields]
  _ -> fail $ "LibPandoc.SchemaTH: unsupported constructor " ++ show con
 where
  build n fields = do
    described <- mapM (describeType . snd) fields
    let fieldVals = [ object ["name" .= fname, "type" .= t]
                    | ((fname, _), (t, _)) <- zip fields described ]
    pure (object ["name" .= nameBase n, "fields" .= fieldVals],
          concatMap snd described)

-- | A type expression, and the pandoc-types declarations it refers to.
describeType :: Type -> Q (Value, [Name])
describeType ty = case ty of
  AppT ListT t -> wrap1 "list" t
  AppT (ConT m) t | nameBase m == "Maybe" -> wrap1 "maybe" t
  AppT (AppT (ConT m) k) v | nameBase m == "Map" -> do
    (k', r1) <- describeType k
    (v', r2) <- describeType v
    pure (object ["map" .= [k', v']], r1 ++ r2)
  _ | (TupleT n, args) <- splitApp ty, n == length args, n > 1 -> do
    (ts, refs) <- unzip <$> mapM describeType args
    pure (object ["tuple" .= ts], concat refs)
  ConT n
    | nameModule n == Just definitionModule ->
        pure (object ["ref" .= nameBase n], [n])
    | Just p <- lookup (nameBase n) primitives ->
        pure (object ["prim" .= (p :: String)], [])
  _ -> fail $ "LibPandoc.SchemaTH: unsupported type " ++ pprint ty
 where
  wrap1 key t = do
    (t', refs) <- describeType t
    pure (object [Key.fromString key .= t'], refs)
  splitApp (AppT f x) = let (h, xs) = splitApp f in (h, xs ++ [x])
  splitApp t = (t, [])

primitives :: [(String, String)]
primitives =
  [ ("Text", "string")
  , ("Int", "int")
  , ("Double", "double")
  , ("Bool", "bool")
  ]
