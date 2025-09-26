//===- DriverDispatcher.cpp - Support using LLD as a library --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lld/Common/CommonLinkerContext.h"
#include "lld/Common/Driver.h"
#include "lld/Common/DriverDispatcher.h"
#include "lld/Common/ErrorHandler.h"
#include "lld/Common/Memory.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/CrashRecoveryContext.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"
#include <cstdlib>
#include <iostream>

using namespace lld;
using namespace llvm;
using namespace llvm::sys;

static void err(const Twine &s) { llvm::errs() << s << "\n"; }

Flavor DriverDispatcher::getFlavor(StringRef s) {
  return StringSwitch<Flavor>(s)
      .CasesLower("ld", "ld.lld", "gnu", Gnu)
      .CasesLower("wasm", "ld-wasm", Wasm)
      .CaseLower("link", WinLink)
      .CasesLower("ld64", "ld64.lld", "darwin", Darwin)
      .Default(Invalid);
}

cl::TokenizerCallback DriverDispatcher::getDefaultQuotingStyle() {
  if (Triple(sys::getProcessTriple()).getOS() == Triple::Win32)
    return cl::TokenizeWindowsCommandLine;
  return cl::TokenizeGNUCommandLine;
}

bool DriverDispatcher::isPETargetName(StringRef s) {
  return s == "i386pe" || s == "i386pep" || s == "thumb2pe" || s == "arm64pe" ||
         s == "arm64ecpe" || s == "arm64xpe" || s == "mipspe";
}

std::optional<bool> DriverDispatcher::isPETarget(llvm::ArrayRef<const char *> args) {
  for (auto it = args.begin(); it + 1 != args.end(); ++it) {
    if (StringRef(*it) != "-m")
      continue;
    return isPETargetName(*(it + 1));
  }

  // Expand response files (arguments in the form of @<filename>)
  // to allow detecting the -m argument from arguments in them.
  SmallVector<const char *, 256> expandedArgs(args.data(),
                                              args.data() + args.size());
  BumpPtrAllocator a;
  StringSaver saver(a);
  cl::ExpansionContext ectx(saver.getAllocator(), getDefaultQuotingStyle());
  if (Error e = ectx.expandResponseFiles(expandedArgs)) {
    err(toString(std::move(e)));
    return std::nullopt;
  }

  for (auto it = expandedArgs.begin(); it + 1 != expandedArgs.end(); ++it) {
    if (StringRef(*it) != "-m")
      continue;
    return isPETargetName(*(it + 1));
  }

#ifdef LLD_DEFAULT_LD_LLD_IS_MINGW
  return true;
#else
  return false;
#endif
}

Flavor DriverDispatcher::parseProgname(StringRef progname) {
  // Use GNU driver for "ld" by default.
  if (progname == "ld")
    return Gnu;

  // Progname may be something like "lld-gnu". Parse it.
  SmallVector<StringRef, 3> v;
  progname.split(v, "-");
  for (StringRef s : v)
    if (Flavor f = getFlavor(s))
      return f;
  return Invalid;
}

Flavor DriverDispatcher::parseFlavorWithoutMinGW(llvm::SmallVectorImpl<const char *> &argsV) {
  // Parse -flavor option.
  if (argsV.size() > 1 && argsV[1] == StringRef("-flavor")) {
    if (argsV.size() <= 2) {
      err("missing arg value for '-flavor'");
      return Invalid;
    }
    Flavor f = getFlavor(argsV[2]);
    if (f == Invalid) {
      err("Unknown flavor: " + StringRef(argsV[2]));
      return Invalid;
    }
    argsV.erase(argsV.begin() + 1, argsV.begin() + 3);
    return f;
  }

  // Deduct the flavor from argv[0].
  StringRef arg0 = path::filename(argsV[0]);
  arg0.consume_back_insensitive(".exe");
  Flavor f = parseProgname(arg0);
  if (f == Invalid) {
    err("lld is a generic driver.\n"
        "Invoke ld.lld (Unix), ld64.lld (macOS), lld-link (Windows), wasm-ld"
        " (WebAssembly) instead");
    return Invalid;
  }
  return f;
}

Flavor DriverDispatcher::parseFlavor(llvm::SmallVectorImpl<const char *> &argsV) {
  Flavor f = parseFlavorWithoutMinGW(argsV);
  if (f == Gnu) {
    auto isPE = isPETarget(argsV);
    if (!isPE)
      return Invalid;
    if (*isPE)
      return MinGW;
  }
  return f;
}

Driver DriverDispatcher::whichDriver(llvm::SmallVectorImpl<const char *> &argsV) {
  Flavor f = parseFlavor(argsV);
  auto it =
      llvm::find_if(drivers, [=](auto &driverdef) { return driverdef.f == f; });
  if (it == drivers.end()) {
    std::cout << "Driver is invalid or not available in this build." << std::endl;
    // Driver is invalid or not available in this build.
    return [](llvm::ArrayRef<const char *>, llvm::raw_ostream &,
              llvm::raw_ostream &, bool, bool) { return false; };
  }
  return it->d;
}

DriverDispatcher::DriverDispatcher(llvm::ArrayRef<lld::DriverDef> drivers) {
  this->drivers = std::vector<DriverDef>(drivers);
}

namespace lld {
bool inTestOutputDisabled = false;
} // namespace lld


bool DriverDispatcher::unsafeDispatch(llvm::ArrayRef<const char *> args, llvm::raw_ostream &stdoutOS, llvm::raw_ostream &stderrOS, bool exitEarly) {
  SmallVector<const char *, 256> argsV(args);
  Driver d = whichDriver(argsV);
  // Run the driver. If an error occurs, false will be returned.
  int r = !d(argsV, stdoutOS, stderrOS, exitEarly, inTestOutputDisabled);
  // At this point 'r' is either 1 for error, and 0 for no error.

  // Call exit() if we can to avoid calling destructors.
  if (exitEarly)
    exitLld(r);

  // Delete the global context and clear the global context pointer, so that it
  // cannot be accessed anymore.
  CommonLinkerContext::destroy();

  return r;
}

Result DriverDispatcher::dispatch(llvm::ArrayRef<const char *> args, llvm::raw_ostream &stdoutOS, llvm::raw_ostream &stderrOS) {
  bool r = false;
  {
    // The crash recovery is here only to be able to recover from arbitrary
    // control flow when fatal() is called (through setjmp/longjmp or
    // __try/__except).
    llvm::CrashRecoveryContext crc;
    if (!crc.RunSafely([&]() {
          r = unsafeDispatch(args, stdoutOS, stderrOS,
                             /*exitEarly=*/false);
        }))
      return {crc.RetCode, /*canRunAgain=*/false};
  }

  // Cleanup memory and reset everything back in pristine condition. This path
  // is only taken when LLD is in test, or when it is used as a library.
  llvm::CrashRecoveryContext crc;
  if (!crc.RunSafely([&]() { CommonLinkerContext::destroy(); })) {
    // The memory is corrupted beyond any possible recovery.
    return {r, /*canRunAgain=*/false};
  }
  return {r, /*canRunAgain=*/true};
}

namespace lld {
/// Universal linker main(). This linker emulates the gnu, darwin, or
/// windows linker based on the argv[0] or -flavor option.
bool unsafeLldMain(llvm::ArrayRef<const char *> args,
                   llvm::raw_ostream &stdoutOS, llvm::raw_ostream &stderrOS,
                   llvm::ArrayRef<DriverDef> drivers, bool exitEarly) {
  DriverDispatcher dispatcher(drivers);
  return dispatcher.unsafeDispatch(args, stdoutOS, stderrOS, exitEarly);
}
} // namespace lld

Result lld::lldMain(llvm::ArrayRef<const char *> args,
                    llvm::raw_ostream &stdoutOS, llvm::raw_ostream &stderrOS,
                    llvm::ArrayRef<DriverDef> drivers) {
  DriverDispatcher dispatcher(drivers);
  return dispatcher.dispatch(args, stdoutOS, stderrOS);
}
