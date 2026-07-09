cpp-httplib (vendored dependency) -- [AI-MODS]
==============================================

A single-header C++ HTTP/1.1 server & client library.

  Upstream : https://github.com/yhirose/cpp-httplib
  Version  : v0.49.0
  License  : MIT (see LICENSE in this folder)
  Files    : httplib.h (header-only)

Why it is here
--------------
The `rest-ai-server` branch adds an embedded HTTP server to the engine so an
external webserver can drive the alien battlescape AI over REST. cpp-httplib is
header-only, has no build-time dependencies, and needs no OpenSSL for plain HTTP
(we do not define CPPHTTPLIB_OPENSSL_SUPPORT -- security is out of scope here).

How it is consumed
------------------
Only ONE translation unit includes it -- src/Engine/RestAiServer.cpp -- via a
relative path (`#include "../../libs/httplib/httplib.h"`), so no build-system
include-directory edits are required. On MSVC the header self-links ws2_32 via
`#pragma comment(lib, "ws2_32.lib")`; on POSIX it uses libc sockets + pthread.
httplib.h must be included before any header that pulls <windows.h>, which is
why RestAiServer.cpp includes it first.

How to update
-------------
  curl -sL -o httplib.h  https://raw.githubusercontent.com/yhirose/cpp-httplib/<tag>/httplib.h
  curl -sL -o LICENSE    https://raw.githubusercontent.com/yhirose/cpp-httplib/<tag>/LICENSE
then bump the Version line above.
