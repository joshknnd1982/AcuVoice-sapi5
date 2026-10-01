# Notices

The code written for this project is licensed under the MIT License (see [LICENSE.txt](LICENSE.txt)). The material
below is not covered by that licence and stays under its own terms.

## The AcuVoice speech engine

This licence covers the SAPI5 wrapper, the configuration utility, the worker process,
the diagnostics tool and the build files in this repository -- the code written for this
project.

It does NOT cover the AcuVoice speech engine itself: avcore.dll, the recorded sound bank
in Ulaw08Sb, the dictionary files in Dictfls, and the user dictionary editor. Those are
(c) 1998-1999 AcuVoice, Inc., which was acquired by Fonix Corporation in 1998. Fonix's
speech business was wound down in the late 2000s and the product has not been sold, sold
on, or supported since. It is abandonware. No claim of ownership over it is made here,
no licence to it is granted here, and it is not in this repository's source tree for
that reason -- it ships only in the release installer, for the benefit of people who
already have a copy of a product nobody sells any more.

If you hold the rights to the AcuVoice engine and want the binaries taken down, open an
issue on the repository and they will be removed.

## Not covered: the SAPI 5 COM skeleton from the BestSpeech SAPI 5 wrapper

The files below are an exception to the statement above that the licence covers the SAPI5
wrapper. They were adapted from the SAPI 5 COM server and token enumerator skeleton of the
BestSpeech SAPI 5 wrapper by Gozaltech (<https://github.com/gozaltech/BstSpeech-sapi>), they
are not the work of this project's author, and the MIT License does not cover them. They stay
under their original author's terms.

- `src/com.hpp` and `src/com.cpp`
- `src/registry.hpp` and `src/registry.cpp`
- `src/utils.hpp`
- `src/ISpDataKeyImpl.hpp` and `src/ISpDataKeyImpl.cpp`
- `src/IEnumSpObjectTokensImpl.hpp` and `src/IEnumSpObjectTokensImpl.cpp`
- `src/voice_token.hpp` and `src/voice_token.cpp`
