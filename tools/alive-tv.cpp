// Copyright (c) 2018-present The Alive2 Authors.
// Distributed under the MIT license that can be found in the LICENSE file.

#include "llvm_util/llvm2alive.h"
#include "smt/smt.h"
#include "tools/transform.h"
#include "util/version.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/Triple.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/InitializePasses.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <utility>

using namespace tools;
using namespace util;
using namespace std;
using namespace llvm_util;

#define LLVM_ARGS_PREFIX ""
#define ARGS_SRC_TGT
#define ARGS_REFINEMENT
#include "llvm_util/cmd_args_list.h"

namespace {

llvm::cl::opt<string> opt_file1(llvm::cl::Positional,
                                llvm::cl::desc("first_bitcode_file"),
                                llvm::cl::Required,
                                llvm::cl::value_desc("filename"),
                                llvm::cl::cat(alive_cmdargs));

llvm::cl::opt<string> opt_file2(llvm::cl::Positional,
                                llvm::cl::desc("[second_bitcode_file]"),
                                llvm::cl::Optional,
                                llvm::cl::value_desc("filename"),
                                llvm::cl::cat(alive_cmdargs));

llvm::cl::opt<std::string>
    opt_src_fn(LLVM_ARGS_PREFIX "src-fn",
               llvm::cl::desc("Name of src function (without @)"),
               llvm::cl::cat(alive_cmdargs), llvm::cl::init("src"));

llvm::cl::opt<std::string>
    opt_tgt_fn(LLVM_ARGS_PREFIX "tgt-fn",
               llvm::cl::desc("Name of tgt function (without @)"),
               llvm::cl::cat(alive_cmdargs), llvm::cl::init("tgt"));

llvm::ExitOnError ExitOnErr;

// adapted from llvm-dis.cpp
std::unique_ptr<llvm::Module> openInputFile(llvm::LLVMContext &Context,
                                            const string &InputFilename) {
  auto MB =
      ExitOnErr(errorOrToExpected(llvm::MemoryBuffer::getFile(InputFilename)));
  llvm::SMDiagnostic Diag;
  auto M = getLazyIRModule(std::move(MB), Diag, Context,
                           /*ShouldLazyLoadMetadata=*/true);
  if (!M) {
    Diag.print("", llvm::errs(), false);
    return 0;
  }
  ExitOnErr(M->materializeAll());
  return M;
}

optional<smt::smt_initializer> smt_init;

struct Results {
  Transform t;
  string error;
  Errors errs;
  enum {
    ERROR,
    TYPE_CHECKER_FAILED,
    SYNTACTIC_EQ,
    CORRECT,
    UNSOUND,
    FAILED_TO_PROVE
  } status;

  static Results Error(string &&err) {
    Results r;
    r.status = ERROR;
    r.error = std::move(err);
    return r;
  }
};

Results verify(llvm::Function &F1, llvm::Function &F2,
               llvm::TargetLibraryInfoWrapperPass &TLI,
               bool print_transform = false, bool always_verify = false) {
  auto fn1 = llvm2alive(F1, TLI.getTLI(F1));
  if (!fn1)
    return Results::Error("Could not translate '" + F1.getName().str() +
                          "' to Alive IR\n");

  auto fn2 = llvm2alive(F2, TLI.getTLI(F2), fn1->getGlobalVarNames());
  if (!fn2)
    return Results::Error("Could not translate '" + F2.getName().str() +
                          "' to Alive IR\n");

  Results r;
  r.t.src = std::move(*fn1);
  r.t.tgt = std::move(*fn2);

  if (!always_verify) {
    stringstream ss1, ss2;
    r.t.src.print(ss1);
    r.t.tgt.print(ss2);
    if (ss1.str() == ss2.str()) {
      if (print_transform)
        r.t.print(*out, {});
      r.status = Results::SYNTACTIC_EQ;
      return r;
    }
  }

  smt_init->reset();
  r.t.preprocess();
  TransformVerify verifier(r.t, false);

  if (print_transform)
    r.t.print(*out, {});

  {
    auto types = verifier.getTypings();
    if (!types) {
      r.status = Results::TYPE_CHECKER_FAILED;
      return r;
    }
    assert(types.hasSingleTyping());
  }

  r.errs = verifier.verify();
  if (r.errs) {
    r.status = r.errs.isUnsound() ? Results::UNSOUND : Results::FAILED_TO_PROVE;
  } else {
    r.status = Results::CORRECT;
  }
  return r;
}

unsigned num_correct = 0;
unsigned num_unsound = 0;
unsigned num_failed = 0;
unsigned num_errors = 0;

bool compareFunctions(llvm::Function &F1, llvm::Function &F2,
                      llvm::TargetLibraryInfoWrapperPass &TLI) {
  auto r = verify(F1, F2, TLI, !opt_quiet, opt_always_verify);
  if (r.status == Results::ERROR) {
    *out << "ERROR: " << r.error;
    ++num_errors;
    return true;
  }

  if (opt_print_dot) {
    r.t.src.writeDot("src");
    r.t.tgt.writeDot("tgt");
  }

  switch (r.status) {
  case Results::ERROR:
    UNREACHABLE();
    break;

  case Results::SYNTACTIC_EQ:
    *out << "Transformation seems to be correct! (syntactically equal)\n\n";
    ++num_correct;
    break;

  case Results::CORRECT:
    *out << "Transformation seems to be correct!\n\n";
    ++num_correct;
    break;

  case Results::TYPE_CHECKER_FAILED:
    *out << "Transformation doesn't verify!\n"
            "ERROR: program doesn't type check!\n\n";
    ++num_errors;
    return true;

  case Results::UNSOUND:
    *out << "Transformation doesn't verify!\n\n";
    if (!opt_quiet)
      *out << r.errs << endl;
    ++num_unsound;
    return false;

  case Results::FAILED_TO_PROVE:
    *out << r.errs << endl;
    ++num_failed;
    return true;
  }

  if (opt_bidirectional) {
    r = verify(F2, F1, TLI, false, opt_always_verify);
    switch (r.status) {
    case Results::ERROR:
    case Results::TYPE_CHECKER_FAILED:
      UNREACHABLE();
      break;

    case Results::SYNTACTIC_EQ:
    case Results::CORRECT:
      *out << "These functions seem to be equivalent!\n\n";
      return true;

    case Results::FAILED_TO_PROVE:
      *out << "Failed to verify the reverse transformation\n\n";
      if (!opt_quiet)
        *out << r.errs << endl;
      return true;

    case Results::UNSOUND:
      *out << "Reverse transformation doesn't verify!\n\n";
      if (!opt_quiet)
        *out << r.errs << endl;
      return false;
    }
  }
  return true;
}

void optimizeModule(llvm::Module *M) {
  llvm::LoopAnalysisManager LAM;
  llvm::FunctionAnalysisManager FAM;
  llvm::CGSCCAnalysisManager CGAM;
  llvm::ModuleAnalysisManager MAM;

  llvm::PassBuilder PB;
  PB.registerModuleAnalyses(MAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

  llvm::FunctionPassManager FPM = PB.buildFunctionSimplificationPipeline(
      llvm::OptimizationLevel::O2, llvm::ThinOrFullLTOPhase::None);
  llvm::ModulePassManager MPM;
  MPM.addPass(createModuleToFunctionPassAdaptor(std::move(FPM)));
  MPM.run(*M, MAM);
}

llvm::Function *findFunction(llvm::Module &M, const string &FName) {
  for (auto &F : M) {
    if (F.isDeclaration())
      continue;
    if (FName.compare(F.getName()) != 0)
      continue;
    return &F;
  }
  return 0;
}

llvm::Align calculatePtrTypeAlign(llvm::Type *pty) {
  switch (pty->getPointerElementType()->getIntegerBitWidth()) {
  case 8:
    return llvm::Align(1);
  case 16:
    return llvm::Align(2);
  case 32:
    return llvm::Align(4);
  case 64:
    return llvm::Align(8);
  default:
    // will break at LoadInst anyway...
    return llvm::Align(1);
  }
}

bool checkIntrinsicTypeMatch(llvm::CallInst *CI) {
  auto ty = CI->getType();
  auto bw = ty->getIntegerBitWidth();
  auto bw_str = std::to_string(bw);
  // not very robust check, but should be sufficient for most of the cases...
  return CI->getCalledFunction()->getName().endswith(bw_str);
}

#define CHECK_INTRINSIC(CI)                                                    \
  {                                                                            \
    if (!checkIntrinsicTypeMatch(CI)) {                                        \
      llvm::errs() << "Warning: ill-named SWPP intrinsic ";                    \
      CI->getCalledFunction()->print(llvm::errs());                            \
      continue;                                                                \
    }                                                                          \
  }

// handle @aload_<Ty>(Ty*) -> Ty
void replaceAsync(llvm::BasicBlock &BB) {
  bool complete_iter = true;
  do {
    complete_iter = true;
    for (auto &I : BB) {
      auto CI = llvm::dyn_cast<llvm::CallInst>(&I);
      if (CI) {
        if (CI->getCalledFunction()->getName().startswith("aload")) {
          CHECK_INTRINSIC(CI);

          auto ptr = CI->getArgOperand(0);
          auto ty = CI->getType();
          auto name = CI->getName();
          auto align = calculatePtrTypeAlign(ptr->getType());

          auto loadInst = new llvm::LoadInst(ty, ptr, name, false, align);
          llvm::ReplaceInstWithInst(CI, loadInst);
          complete_iter = false;
          break;
        }
      }
    }
  } while (!complete_iter);
}

// handle @int_sum_<Ty>(Ty, Ty, Ty, Ty, Ty, Ty, Ty, Ty) -> Ty
void replaceIntSum(llvm::BasicBlock &BB) {
  bool complete_iter = true;
  do {
    complete_iter = true;
    for (auto &I : BB) {
      auto CI = llvm::dyn_cast<llvm::CallInst>(&I);
      if (CI) {
        if (CI->getCalledFunction()->getName().startswith("int_sum")) {
          CHECK_INTRINSIC(CI);

          auto op1 = CI->getArgOperand(0);
          auto op2 = CI->getArgOperand(1);
          auto op3 = CI->getArgOperand(2);
          auto op4 = CI->getArgOperand(3);
          auto op5 = CI->getArgOperand(4);
          auto op6 = CI->getArgOperand(5);
          auto op7 = CI->getArgOperand(6);
          auto op8 = CI->getArgOperand(7);

          auto add1 = llvm::BinaryOperator::Create(llvm::Instruction::Add, op1,
                                                   op2, llvm::Twine(), CI);
          auto add2 = llvm::BinaryOperator::Create(llvm::Instruction::Add, add1,
                                                   op3, llvm::Twine(), CI);
          auto add3 = llvm::BinaryOperator::Create(llvm::Instruction::Add, add2,
                                                   op4, llvm::Twine(), CI);
          auto add4 = llvm::BinaryOperator::Create(llvm::Instruction::Add, add3,
                                                   op5, llvm::Twine(), CI);
          auto add5 = llvm::BinaryOperator::Create(llvm::Instruction::Add, add4,
                                                   op6, llvm::Twine(), CI);
          auto add6 = llvm::BinaryOperator::Create(llvm::Instruction::Add, add5,
                                                   op7, llvm::Twine(), CI);
          auto add7 =
              llvm::BinaryOperator::Create(llvm::Instruction::Add, add6, op8);

          llvm::ReplaceInstWithInst(CI, add7);
          complete_iter = false;
          break;
        }
      }
    }
  } while (!complete_iter);
}

// handle @incr_<Ty>(Ty) -> Ty
void replaceIncrement(llvm::BasicBlock &BB) {
  bool complete_iter = true;
  do {
    complete_iter = true;
    for (auto &I : BB) {
      auto CI = llvm::dyn_cast<llvm::CallInst>(&I);
      if (CI) {
        if (CI->getCalledFunction()->getName().startswith("incr")) {
          CHECK_INTRINSIC(CI);

          auto op = CI->getArgOperand(0);
          auto ty = CI->getType();

          auto addInst = llvm::BinaryOperator::Create(
              llvm::Instruction::Add, op,
              llvm::Constant::getIntegerValue(
                  ty, llvm::APInt(ty->getIntegerBitWidth(), 1)));

          llvm::ReplaceInstWithInst(CI, addInst);
          complete_iter = false;
          break;
        }
      }
    }
  } while (!complete_iter);
}

// handle @decr_<Ty>(Ty) -> Ty
void replaceDecrement(llvm::BasicBlock &BB) {
  bool complete_iter = true;
  do {
    complete_iter = true;
    for (auto &I : BB) {
      auto CI = llvm::dyn_cast<llvm::CallInst>(&I);
      if (CI) {
        if (CI->getCalledFunction()->getName().startswith("decr")) {
          CHECK_INTRINSIC(CI);

          auto op = CI->getArgOperand(0);
          auto ty = CI->getType();
          auto addInst = llvm::BinaryOperator::Create(
              llvm::Instruction::Sub, op,
              llvm::Constant::getIntegerValue(
                  ty, llvm::APInt(ty->getIntegerBitWidth(), 1)));

          llvm::ReplaceInstWithInst(CI, addInst);
          complete_iter = false;
          break;
        }
      }
    }
  } while (!complete_iter);
}

// handle intrinsics added for swpp
void replaceSWPPIntrinsics(llvm::BasicBlock &BB) {
  replaceAsync(BB);
  replaceIntSum(BB);
  replaceIncrement(BB);
  replaceDecrement(BB);
}
} // namespace

int main(int argc, char **argv) {
  llvm::sys::PrintStackTraceOnErrorSignal(argv[0]);
  llvm::PrettyStackTraceProgram X(argc, argv);
  llvm::EnableDebugBuffering = true;
  llvm::llvm_shutdown_obj llvm_shutdown; // Call llvm_shutdown() on exit.
  llvm::LLVMContext Context;

  std::string Usage =
      R"EOF(Alive2 stand-alone translation validator:
version )EOF";
  Usage += alive_version;
  Usage += R"EOF(
see alive-tv --version  for LLVM version info,

This program takes either one or two LLVM IR files files as
command-line arguments. Both .bc and .ll files are supported.

If two files are provided, alive-tv checks that functions in the
second file refine functions in the first file, matching up functions
by name. Functions not found in both files are ignored. It is an error
for a function to be found in both files unless they have the same
signature.

If one file is provided, there are two possibilities. If the file
contains a function called "src" and also a function called "tgt",
then alive-tv will determine whether src is refined by tgt. It is an
error if src and tgt do not have the same signature. Otherwise,
alive-tv will optimize the entire module using an optimization
pipeline similar to -O2, and then verify that functions in the
optimized module refine those in the original one. This provides a
convenient way to demonstrate an existing optimizer bug.
)EOF";

  llvm::cl::HideUnrelatedOptions(alive_cmdargs);
  llvm::cl::ParseCommandLineOptions(argc, argv, Usage);

  auto M1 = openInputFile(Context, opt_file1);
  if (!M1.get()) {
    cerr << "Could not read bitcode from '" << opt_file1 << "'\n";
    return -1;
  }
  for (auto &F : *M1.get()) {
    for (auto &BB : F) {
      replaceSWPPIntrinsics(BB);
    }
  }

#define ARGS_MODULE_VAR M1
#include "llvm_util/cmd_args_def.h"

  auto &DL = M1.get()->getDataLayout();
  llvm::Triple targetTriple(M1.get()->getTargetTriple());
  llvm::TargetLibraryInfoWrapperPass TLI(targetTriple);

  llvm_util::initializer llvm_util_init(*out, DL);
  smt_init.emplace();

  unique_ptr<llvm::Module> M2;
  if (opt_file2.empty()) {
    auto SRC = findFunction(*M1, opt_src_fn);
    auto TGT = findFunction(*M1, opt_tgt_fn);
    if (SRC && TGT) {
      compareFunctions(*SRC, *TGT, TLI);
      goto end;
    } else {
      M2 = CloneModule(*M1);
      optimizeModule(M2.get());
    }
  } else {
    M2 = openInputFile(Context, opt_file2);
    if (!M2.get()) {
      *out << "Could not read bitcode from '" << opt_file2 << "'\n";
      return -1;
    }
    for (auto &F : *M2.get()) {
      for (auto &BB : F) {
        replaceSWPPIntrinsics(BB);
      }
    }
  }

  if (M1.get()->getTargetTriple() != M2.get()->getTargetTriple()) {
    *out << "Modules have different target triples\n";
    return -1;
  }

  // FIXME: quadratic, may not be suitable for very large modules
  // emitted by opt-fuzz
  for (auto &F1 : *M1.get()) {
    if (F1.isDeclaration())
      continue;
    if (!func_names.empty() && !func_names.count(F1.getName().str()))
      continue;
    for (auto &F2 : *M2.get()) {
      if (F2.isDeclaration() || F1.getName() != F2.getName())
        continue;
      if (!compareFunctions(F1, F2, TLI))
        if (opt_error_fatal)
          goto end;
      break;
    }
  }

  *out << "Summary:\n"
          "  "
       << num_correct
       << " correct transformations\n"
          "  "
       << num_unsound
       << " incorrect transformations\n"
          "  "
       << num_failed
       << " failed-to-prove transformations\n"
          "  "
       << num_errors << " Alive2 errors\n";

end:
  if (opt_smt_stats)
    smt::solver_print_stats(*out);

  smt_init.reset();

  if (opt_alias_stats)
    IR::Memory::printAliasStats(*out);

  return num_errors > 0;
}
