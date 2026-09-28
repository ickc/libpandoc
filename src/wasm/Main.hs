-- | libpandoc.wasm is a reactor: the host calls the exported C functions
-- (cbits/libpandoc.c). GHC wants a main all the same; it is never run.
module Main (main) where

import LibPandoc ()

main :: IO ()
main = pure ()
