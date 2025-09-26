#include "lld/Common/Driver.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

namespace lld {

class DriverDispatcher {
public:
    DriverDispatcher(llvm::ArrayRef<lld::DriverDef> drivers);
    bool unsafeDispatch(llvm::ArrayRef<const char *> args, llvm::raw_ostream &stdoutOS, llvm::raw_ostream &stderrOS, bool exitEarly);
    Result dispatch(llvm::ArrayRef<const char *> args, llvm::raw_ostream &stdoutOS, llvm::raw_ostream &stderrOS);
private:
    std::vector<DriverDef> drivers;
    static Flavor getFlavor(StringRef s);
    static llvm::cl::TokenizerCallback getDefaultQuotingStyle();
    static bool isPETargetName(StringRef s);
    static std::optional<bool> isPETarget(llvm::ArrayRef<const char*> args);
    static Flavor parseProgname(StringRef progname);
    static Flavor parseFlavorWithoutMinGW(llvm::SmallVectorImpl<const char*> &argsV);
    static Flavor parseFlavor(llvm::SmallVectorImpl<const char *> &argsV);
    Driver whichDriver(llvm::SmallVectorImpl<const char *> &argsV);
};

} // namespace lld