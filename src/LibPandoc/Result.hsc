{- |
   Module      : LibPandoc.Result
   Copyright   : Copyright (C) 2026 Kolen Cheung
   License     : GNU GPL, version 2 or above

Builds the @pandoc_result@ struct of @libpandoc.h@. Field offsets come from
the header through hsc2hs, so the struct is defined in one place only.
-}
module LibPandoc.Result
  ( Result (..)
  , newResult
  ) where

import qualified Data.ByteString as B
import qualified Data.ByteString.Unsafe as BU
import Foreign
import Foreign.C

#include "libpandoc.h"

data Result = Result
  { resultOutput :: B.ByteString
  , resultError  :: Maybe (B.ByteString, B.ByteString) -- ^ kind, message
  , resultLog    :: B.ByteString                       -- ^ JSON array
  }

-- | Copy a result into a malloc'd @pandoc_result@, which C frees with
-- @pandoc_result_free@.
newResult :: Result -> IO (Ptr ())
newResult r = do
  p <- callocBytes (#size pandoc_result)
  (out, outLen) <- mallocNul (resultOutput r)
  (#poke pandoc_result, output) p out
  (#poke pandoc_result, output_len) p (fromIntegral outLen :: CSize)
  (#poke pandoc_result, status) p (maybe 0 (const 1) (resultError r) :: CInt)
  case resultError r of
    Nothing -> pure ()
    Just (kind, msg) -> do
      (#poke pandoc_result, error_kind) p . fst =<< mallocNul kind
      (#poke pandoc_result, error_message) p . fst =<< mallocNul msg
  (#poke pandoc_result, log) p . fst =<< mallocNul (resultLog r)
  pure p

-- | A malloc'd copy of the bytes followed by a NUL.
mallocNul :: B.ByteString -> IO (Ptr CChar, Int)
mallocNul bs = BU.unsafeUseAsCStringLen bs $ \(src, len) -> do
  dst <- mallocBytes (len + 1)
  copyBytes dst src len
  pokeByteOff dst len (0 :: Word8)
  pure (dst, len)
