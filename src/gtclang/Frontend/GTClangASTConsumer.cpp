//===--------------------------------------------------------------------------------*- C++ -*-===//
//                         _       _
//                        | |     | |
//                    __ _| |_ ___| | __ _ _ __   __ _
//                   / _` | __/ __| |/ _` | '_ \ / _` |
//                  | (_| | || (__| | (_| | | | | (_| |
//                   \__, |\__\___|_|\__,_|_| |_|\__, | - GridTools Clang DSL
//                    __/ |                       __/ |
//                   |___/                       |___/
//
//
//  This file is distributed under the MIT License (MIT).
//  See LICENSE.txt for details.
//
//===------------------------------------------------------------------------------------------===//

#include "gtclang/Frontend/GTClangASTConsumer.h"
#include "dawn/Compiler/DawnCompiler.h"
#include "dawn/SIR/SIR.h"
#include "dawn/SIR/SIRSerializer.h"
#include "dawn/Support/Config.h"
#include "dawn/Support/Format.h"
#include "dawn/Support/StringUtil.h"
#include "gtclang/Frontend/ClangFormat.h"
#include "gtclang/Frontend/GTClangASTAction.h"
#include "gtclang/Frontend/GTClangASTVisitor.h"
#include "gtclang/Frontend/GTClangContext.h"
#include "gtclang/Frontend/GlobalVariableParser.h"
#include "gtclang/Frontend/StencilParser.h"
#include "gtclang/Support/Config.h"
#include "gtclang/Support/FileUtil.h"
#include "gtclang/Support/Logger.h"
#include "gtclang/Support/StringUtil.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/Version.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include <cstdio>
#include <ctime>
#include <iostream>
#include <memory>
#include <system_error>

namespace gtclang {

/// @brief Get current time-stamp
static const std::string currentDateTime() {
  std::time_t now = time(0);
  char buf[80];
  std::strftime(buf, sizeof(buf), "%Y-%m-%d  %X", std::localtime(&now));
  return buf;
}

/// @brief Extract the DAWN options from the GTClang options
static std::unique_ptr<dawn::Options> makeDAWNOptions(const Options& options) {
  auto DAWNOptions = llvm::make_unique<dawn::Options>();
#define OPT(TYPE, NAME, DEFAULT_VALUE, OPTION, OPTION_SHORT, HELP, VALUE_NAME, HAS_VALUE, F_GROUP) \
  DAWNOptions->NAME = options.NAME;
#include "dawn/Compiler/Options.inc"
#undef OPT
  return DAWNOptions;
}

GTClangASTConsumer::GTClangASTConsumer(GTClangContext* context, const std::string& file,
                                       GTClangASTAction* parentAction)
    : context_(context), file_(file), parentAction_(parentAction) {
  DAWN_LOG(INFO) << "Creating ASTVisitor ... ";
  visitor_ = llvm::make_unique<GTClangASTVisitor>(context);
}

void GTClangASTConsumer::HandleTranslationUnit(clang::ASTContext& ASTContext) {
  context_->setASTContext(&ASTContext);
  if(!context_->hasDiagnostics())
    context_->setDiagnostics(&ASTContext.getDiagnostics());

  DAWN_LOG(INFO) << "Parsing translation unit... ";

  clang::TranslationUnitDecl* TU = ASTContext.getTranslationUnitDecl();
  for(auto& decl : TU->decls())
    visitor_->TraverseDecl(decl);

  DAWN_LOG(INFO) << "Done parsing translation unit";

  if(context_->getASTContext().getDiagnostics().hasErrorOccurred()) {
    DAWN_LOG(INFO) << "Erros occurred. Aborting";
    return;
  }

  DAWN_LOG(INFO) << "Generating SIR ... ";
  auto& SM = context_->getSourceManager();

  // Assemble SIR
  std::shared_ptr<dawn::SIR> SIR = std::make_shared<dawn::SIR>();
  SIR->Filename = SM.getFileEntryForID(SM.getMainFileID())->getName();

  const StencilParser& stencilParser = visitor_->getStencilParser();

  for(const auto& stencilPair : stencilParser.getStencilMap()) {
    SIR->Stencils.emplace_back(stencilPair.second);
    SIR->Stencils.back()->Attributes = context_->getStencilAttribute(stencilPair.second->Name);
  }

  for(const auto& stencilPair : stencilParser.getStencilFunctionMap()) {
    SIR->StencilFunctions.emplace_back(stencilPair.second);
    SIR->StencilFunctions.back()->Attributes =
        context_->getStencilAttribute(stencilPair.second->Name);
  }

  const GlobalVariableParser& globalsParser = visitor_->getGlobalVariableParser();
  SIR->GlobalVariableMap = globalsParser.getGlobalVariableMap();

  if(context_->getOptions().DumpSIR) {
    SIR->dump();
  }

  parentAction_->setSIR(SIR);

  // Set the backend
  dawn::DawnCompiler::CodeGenKind codeGen = dawn::DawnCompiler::CG_GTClang;
  if(context_->getOptions().Backend == "gridtools")
    codeGen = dawn::DawnCompiler::CG_GTClang;
  else if(context_->getOptions().Backend == "c++")
    codeGen = dawn::DawnCompiler::CG_GTClangNaiveCXX;
  else {
    context_->getDiagnostics().report(Diagnostics::err_invalid_option)
        << ("-backend=" + context_->getOptions().Backend)
        << dawn::RangeToString(", ", "", "")(std::vector<std::string>{"gridtools", "c++"});
  }

  // Compile the SIR to GridTools
  dawn::DawnCompiler Compiler(makeDAWNOptions(context_->getOptions()).get());
  std::unique_ptr<dawn::TranslationUnit> DawnTranslationUnit = Compiler.compile(SIR.get(), codeGen);

  // Report diagnostics from Dawn
  if(Compiler.getDiagnostics().hasDiags()) {
    for(const auto& diags : Compiler.getDiagnostics().getQueue()) {
      context_->getDiagnostics().report(*diags);
    }
  }

  if(!DawnTranslationUnit || Compiler.getDiagnostics().hasErrors()) {
    DAWN_LOG(INFO) << "Errors in Dawn. Aborting";
    return;
  }

  // Determine filename of generated file (by default we append "_gen" to the filename)
  std::string generatedFilename;
  if(context_->getOptions().OutputFile.empty())
    generatedFilename = llvm::StringRef(file_).split(".cpp").first.str() + "_gen.cpp";
  else {
    const auto& OutputFile = context_->getOptions().OutputFile;
    llvm::SmallVector<char, 40> path(OutputFile.data(), OutputFile.data() + OutputFile.size());
    llvm::sys::fs::make_absolute(path);
    generatedFilename.assign(path.data(), path.size());
  }

  if(context_->getOptions().WriteSIR) {
    llvm::SmallVector<char, 40> filename = llvm::SmallVector<char, 40>(
        generatedFilename.data(), generatedFilename.data() + generatedFilename.size());

    llvm::sys::path::replace_extension(filename, llvm::Twine(".sir"));

    std::string generatedSIR(filename.data(), filename.data() + filename.size());
    DAWN_LOG(INFO) << "Generating SIR file " << generatedFilename;

    dawn::SIRSerializer::serialize(generatedSIR, SIR.get());
  }

  // Do we generate code?
  if(!context_->getOptions().CodeGen) {
    DAWN_LOG(INFO) << "Skipping code-generation";
    return;
  }

  // Create new in-memory FS
  llvm::IntrusiveRefCntPtr<clang::vfs::InMemoryFileSystem> memFS(
      new clang::vfs::InMemoryFileSystem);
  clang::FileManager files(clang::FileSystemOptions(), memFS);
  clang::SourceManager sources(context_->getASTContext().getDiagnostics(), files);

  // Get a copy of the main-file's code
  std::unique_ptr<llvm::MemoryBuffer> generatedCode =
      llvm::MemoryBuffer::getMemBufferCopy(SM.getBufferData(SM.getMainFileID()));

  // Create the generated file
  DAWN_LOG(INFO) << "Creating generated file " << generatedFilename;
  clang::FileID generatedFileID =
      createInMemoryFile(generatedFilename, generatedCode.get(), sources, files, memFS.get());

  // Replace clang DSL with gridtools
  clang::Rewriter rewriter(sources, context_->getASTContext().getLangOpts());
  for(const auto& stencilPair : stencilParser.getStencilMap()) {
    clang::CXXRecordDecl* stencilDecl = stencilPair.first;
    if(rewriter.ReplaceText(stencilDecl->getSourceRange(),
                            stencilPair.second->Attributes.has(dawn::sir::Attr::AK_NoCodeGen)
                                ? "class " + stencilPair.second->Name + "{}"
                                : DawnTranslationUnit->getStencils().at(stencilPair.second->Name)))
      context_->getDiagnostics().report(Diagnostics::err_fs_error) << dawn::format(
          "unable to replace stencil code at: %s", stencilDecl->getLocation().printToString(SM));
  }

  // Replace globals struct
  if(!globalsParser.isEmpty() && !DawnTranslationUnit->getGlobals().empty()) {
    if(rewriter.ReplaceText(globalsParser.getRecordDecl()->getSourceRange(),
                            DawnTranslationUnit->getGlobals()))
      context_->getDiagnostics().report(Diagnostics::err_fs_error)
          << dawn::format("unable to replace globals code at: %s",
                          globalsParser.getRecordDecl()->getLocation().printToString(SM));
  }

  // Remove the code from stencil-functions
  for(const auto& stencilFunPair : stencilParser.getStencilFunctionMap()) {
    clang::CXXRecordDecl* stencilFunDecl = stencilFunPair.first;
    rewriter.ReplaceText(stencilFunDecl->getSourceRange(),
                         "class " + stencilFunPair.second->Name + "{}");
  }

  std::string code;
  llvm::raw_string_ostream os(code);
  rewriter.getEditBuffer(generatedFileID).write(os);
  os.flush();

  // Format the file
  if(context_->getOptions().ClangFormat) {
    ClangFormat clangformat(context_);
    code = clangformat.format(code);
  }

  // Write file to disk
  DAWN_LOG(INFO) << "Writing file to disk... ";
  std::error_code ec;
  llvm::sys::fs::OpenFlags flags = llvm::sys::fs::OpenFlags::F_RW;
  llvm::raw_fd_ostream fout(generatedFilename, ec, flags);

  // Print a header
  fout << dawn::format("// gtclang (%s)\n// based on LLVM/Clang (%s), Dawn (%s)\n",
                       GTCLANG_FULL_VERSION_STR, LLVM_VERSION_STRING, DAWN_VERSION_STR);
  fout << "// Generated on " << currentDateTime() << "\n\n";

  // Add the macro definitions
  for(const auto& macroDefine : DawnTranslationUnit->getPPDefines())
    fout << macroDefine << "\n";

  fout.write(code.data(), code.size());
  if(ec.value())
    context_->getDiagnostics().report(Diagnostics::err_fs_error) << ec.message();
}

} // namespace gtclang
