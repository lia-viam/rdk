#include <viam/generator/generator.hpp>

#include <viam/generator/compilation_db.hpp>
#include <viam/generator/compiler_info.hpp>
#include <viam/generator/template_constants.hpp>

#include <clang/AST/PrettyPrinter.h>
#include <clang/AST/QualTypeNames.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Frontend/FrontendActions.h>
#include <clang/Tooling/JSONCompilationDatabase.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringExtras.h>
#include <llvm/ADT/StringMap.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/raw_ostream.h>

#include <string_view>
#include <unordered_map>

namespace viam::gen {

Generator Generator::create(Generator::ModuleInfo moduleInfo,
                            Generator::CppTreeInfo cppInfo,
                            llvm::raw_ostream& moduleFile) {
    std::string error;
    auto jsonDb =
        clang::tooling::JSONCompilationDatabase::autoDetectFromDirectory(cppInfo.buildDir, error);
    if (!jsonDb) {
        throw std::runtime_error(error);
    }

    return Generator(
        GeneratorCompDB(*jsonDb, getCompilersDefaultIncludeDir(*jsonDb, true)),
        moduleInfo.resourceType,
        moduleInfo.resourceSubtypeSnake.str(),
        (cppInfo.sourceDir +
         resourceToSource(moduleInfo.resourceSubtypeSnake, moduleInfo.resourceType, SrcType::cpp))
            .str(),
        moduleFile);
}

Generator Generator::createFromCommandLine(const clang::tooling::CompilationDatabase& db,
                                           llvm::StringRef sourceFile,
                                           llvm::raw_ostream& outFile) {
    return Generator(GeneratorCompDB(db, getCompilersDefaultIncludeDir(db, true)),
                     to_resource_type((*++llvm::sys::path::rbegin(sourceFile)).drop_back()),
                     llvm::sys::path::stem(sourceFile).str(),
                     sourceFile.str(),
                     outFile);
}

Generator::ResourceType Generator::to_resource_type(llvm::StringRef resourceType) {
    if (resourceType == "component") {
        return ResourceType::component;
    }

    if (resourceType == "service") {
        return ResourceType::service;
    }

    throw std::runtime_error(("Invalid resource type" + resourceType).str());
}

Generator::Generator(GeneratorCompDB db,
                     ResourceType resourceType,
                     std::string resourceSubtypeSnake,
                     std::string resourcePath,
                     llvm::raw_ostream& moduleFile)
    : db_(std::move(db)),
      resourceType_(resourceType),
      resourceSubtypeSnake_(std::move(resourceSubtypeSnake)),
      resourceSubtypePascal_(llvm::convertToCamelFromSnakeCase(resourceSubtypeSnake_, true)),
      resourcePath_(std::move(resourcePath)),
      moduleFile_(moduleFile) {
    if (llvm::StringRef(resourceSubtypeSnake_).startswith("generic_")) {
        resourceSubtypeSnake_ = "generic";
    }
}

int Generator::run() {
    include_stmts();

    moduleFile_ << llvm::formatv("namespace {0} {\n\n", fmt_str::moduleName);

    const char* fmt =
        R"--(
class {0} : public viam::sdk::{1}, public viam::sdk::Reconfigurable {{
public:
    {0}(const viam::sdk::Dependencies& deps, const viam::sdk::ResourceConfig& cfg) : {1}(cfg.name()) {{
        this->reconfigure(deps, cfg);
    }

)--";

    moduleFile_ << llvm::formatv(fmt, fmt_str::modelPascal, resourceSubtypePascal_);

    moduleFile_ << R"--(
    static std::vector<std::string> validate(const viam::sdk::ResourceConfig&)
    {
        throw std::runtime_error("\"validate\" not implemented");
    }

    void reconfigure(const viam::sdk::Dependencies&, const viam::sdk::ResourceConfig&) override
    {
        throw std::runtime_error("\"reconfigure\" not implemented");
    }

)--";

    int result = do_stubs();

    if (result != 0) {
        throw std::runtime_error("Nonzero return from stub generation");
    }

    moduleFile_ << "};\n\n";

    moduleFile_ << llvm::formatv("} // namespace {0} \n", fmt_str::moduleName);

    return 0;
}

template <>
const char* Generator::include_fmt<Generator::ResourceType::component>() {
    constexpr const char* fmt = R"--(
#include <viam/sdk/common/proto_value.hpp>
#include <viam/sdk/{0}>
#include <viam/sdk/config/resource.hpp>
#include <viam/sdk/module/service.hpp>
#include <viam/sdk/resource/reconfigurable.hpp>

)--";

    return fmt;
}

template <>
const char* Generator::include_fmt<Generator::ResourceType::service>() {
    constexpr const char* fmt = R"--(
#include <viam/sdk/common/proto_value.hpp>
#include <viam/sdk/config/resource.hpp>
#include <viam/sdk/module/service.hpp>
#include <viam/sdk/resource/reconfigurable.hpp>
#include <viam/sdk/{0}>

)--";

    return fmt;
}

void Generator::include_stmts() {
    const char* fmt = (resourceType_ == ResourceType::component)
                          ? include_fmt<ResourceType::component>()
                          : include_fmt<ResourceType::service>();

    moduleFile_ << llvm::formatv(
        fmt, resourceToSource(resourceSubtypeSnake_, resourceType_, SrcType::hpp));
}

int Generator::do_stubs() {
    clang::tooling::ClangTool tool(db_, resourcePath_);

    using namespace clang::ast_matchers;

    std::string qualName = ("viam::sdk::" + resourceSubtypePascal_);

    DeclarationMatcher methodMatcher =
        cxxMethodDecl(isPure(), hasParent(cxxRecordDecl(hasName(qualName)))).bind("method");

    struct MethodPrinter : MatchFinder::MatchCallback {
        MethodPrinter(llvm::raw_ostream& os_) : os(os_) {}

        llvm::raw_ostream& os;

        void printParm(const clang::ParmVarDecl& parm) {
            os << clang::TypeName::getFullyQualifiedName(
                      parm.getType(), parm.getASTContext(), {parm.getASTContext().getLangOpts()})
               << " " << parm.getName();
        }

        void run(const MatchFinder::MatchResult& result) override {
            if (const auto* method = result.Nodes.getNodeAs<clang::CXXMethodDecl>("method")) {
                clang::PrintingPolicy printPolicy(method->getASTContext().getLangOpts());
                printPolicy.FullyQualifiedName = 1;

                const std::string& retType = clang::TypeName::getFullyQualifiedName(
                    method->getReturnType(), method->getASTContext(), printPolicy);

                os << "    " << retType << (retType.size() < 70 ? " " : "\n    ")
                   << method->getName() << "(";

                const auto paramCount = method->getNumParams();

                auto printParamBreak = [paramCount, this] {
                    if (paramCount > 1) {
                        os << "\n        ";
                    }
                };

                if (paramCount > 0) {
                    auto param_begin = method->param_begin();

                    printParamBreak();
                    printParm(**param_begin);

                    if (paramCount > 1) {
                        for (const clang::ParmVarDecl* parm :
                             llvm::makeArrayRef(++param_begin, method->param_end())) {
                            os << ",";
                            printParamBreak();
                            printParm(*parm);
                        }
                    }
                }

                os << ")";

                method->getMethodQualifiers().print(os, printPolicy, false);

                os << " override";

                os << llvm::formatv(R"--(
    {
        throw std::logic_error("\"{0}\" not implemented");
    }

)--",
                                    method->getName());
            }
        }
    };

    MethodPrinter printer(moduleFile_);
    MatchFinder finder;

    finder.addMatcher(methodMatcher, &printer);

    return tool.run(clang::tooling::newFrontendActionFactory(&finder).get());
}

void Generator::main_fn(llvm::raw_ostream& moduleFile) {
    moduleFile <<

        llvm::formatv(R"--(
#include "{0}.hpp"

#include <iostream>
#include <memory>
#include <vector>

#include <viam/sdk/common/exception.hpp>
#include <viam/sdk/common/instance.hpp>
#include <viam/sdk/log/logging.hpp>
#include <viam/sdk/registry/registry.hpp>


)--",
                      fmt_str::modelSnake)
               << "int main(int argc, char** argv) try {\n"
               << llvm::formatv(R"--(
    // Every Viam C++ SDK program must have one and only one Instance object which is created before
    // any other SDK objects and stays alive until all of them are destroyed.
    viam::sdk::Instance inst;

    // Write general log statements using the VIAM_SDK_LOG macro.
    VIAM_SDK_LOG(info) << "Starting up {1} module";

    viam::sdk::Model model("{0}", "{1}", "{2}");)--",
                                fmt_str::orgID,
                                fmt_str::moduleName,
                                fmt_str::modelSnake)
               << "\n\n"
               << llvm::formatv(
                      R"--(
    auto mr = std::make_shared<viam::sdk::ModelRegistration>(
        viam::sdk::API::get<viam::sdk::{0}>(),
        model,
        [](viam::sdk::Dependencies deps, viam::sdk::ResourceConfig cfg) {
            return std::make_unique<{1}>(deps, cfg);
        },
        &{1}::validate);
)--",
                      fmt_str::resourceSubtypePascal,
                      fmt_str::moduleName + llvm::Twine("::") + fmt_str::modelPascal)
               << "\n\n"
               <<
        R"--(
    std::vector<std::shared_ptr<viam::sdk::ModelRegistration>> mrs = {mr};
    auto my_mod = std::make_shared<viam::sdk::ModuleService>(argc, argv, mrs);
    my_mod->serve();

    return EXIT_SUCCESS;
} catch (const viam::sdk::Exception& ex) {
    std::cerr << "main failed with exception: " << ex.what() << "\n";
    return EXIT_FAILURE;
}
)--";
}

void Generator::cmakelists(llvm::raw_ostream& outFile) {
    outFile << llvm::formatv(R"--(
cmake_minimum_required(VERSION 3.25 FATAL_ERROR)

project({0}
    DESCRIPTION "Viam C++ {0} Module"
    LANGUAGES CXX
)

# Everything needs threads, and prefer -pthread if available
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

find_package(viam-cpp-sdk CONFIG REQUIRED COMPONENTS viamsdk)

add_executable({0}
    src/main.cpp
    src/{1}.cpp
)

target_include_directories({0} src)

target_link_libraries({0}
    viam-cpp-sdk::viamsdk
)

)--",
                             fmt_str::moduleName,
                             fmt_str::modelSnake);
}

void Generator::conanfile(llvm::raw_ostream& outFile) {
    outFile << llvm::formatv(R"--(
from conan import ConanFile
from conan.tools.cmake import CMakeToolchain, CMake, cmake_layout, CMakeDeps

class {0}Recipe(ConanFile):
    name = "{0}"
    version = "0.1"
    package_type = "application"

    # Optional metadata
    license = "<Put the package license here>"
    author = "<Put your name here> <And your email here>"
    url = "<Package recipe repository url here, for issues about the package>"
    description = "<Description of mysensor package here>"
    topics = ("<Put some tag here>", "<here>", "<and here>")

    # Binary configuration
    settings = "os", "compiler", "build_type", "arch"

    # Sources are located in the same place as this recipe, copy them to the recipe
    exports_sources = "CMakeLists.txt", "src/*"

    def layout(self):
        cmake_layout(self)

    def configure(self):
        self.options["viam-cpp-sdk"].shared = False

    def generate(self):
        deps = CMakeDeps(self)
        deps.generate()
        tc = CMakeToolchain(self)
        tc.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        cmake = CMake(self)
        cmake.install()

    def requirements(self):
        self.requires("viam-cpp-sdk/0.31.0")
)--",
                             fmt_str::moduleName);
}

std::string Generator::resourceToSource(llvm::StringRef resourceSubtype,
                                        Generator::ResourceType resourceType,
                                        Generator::SrcType srcType) {
    if (resourceSubtype.startswith("generic_")) {
        resourceSubtype = "generic";
    }

    return llvm::formatv("{0}/{1}.{2}",
                         (resourceType == ResourceType::component) ? "components" : "services",
                         resourceSubtype,
                         (srcType == SrcType::hpp) ? "hpp" : "cpp");
}

}  // namespace viam::gen
