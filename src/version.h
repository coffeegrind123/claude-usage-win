#pragma once
// The released version is the tag; CI passes it in (-DAPP_VERSION=…), a local build says 0.0.0-dev.
#ifndef APP_VERSION
#define APP_VERSION "0.0.0-dev"
#endif
#define VERSION_STRING APP_VERSION
