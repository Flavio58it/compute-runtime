/*
 * Copyright (C) 2020 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "shared/test/unit_test/compiler_interface/linker_mock.h"
#include "shared/test/unit_test/device_binary_format/zebin_tests.h"

#include "opencl/source/program/kernel_info.h"
#include "test.h"

#include "level_zero/core/source/context/context.h"
#include "level_zero/core/source/kernel/kernel_imp.h"
#include "level_zero/core/source/module/module_imp.h"
#include "level_zero/core/test/unit_tests/fixtures/device_fixture.h"
#include "level_zero/core/test/unit_tests/fixtures/module_fixture.h"
#include "level_zero/core/test/unit_tests/mocks/mock_module.h"

namespace L0 {
namespace ult {

using ModuleTest = Test<ModuleFixture>;

HWTEST_F(ModuleTest, givenBinaryWithDebugDataWhenModuleCreatedFromNativeBinaryThenDebugDataIsStored) {
    size_t size = 0;
    std::unique_ptr<uint8_t[]> data;
    auto result = module->getDebugInfo(&size, nullptr);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    data = std::make_unique<uint8_t[]>(size);
    result = module->getDebugInfo(&size, data.get());
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    EXPECT_NE(nullptr, data.get());
    EXPECT_NE(0u, size);
}

HWTEST_F(ModuleTest, givenKernelCreateReturnsSuccess) {
    ze_kernel_handle_t kernelHandle;

    ze_kernel_desc_t kernelDesc = {};
    kernelDesc.pKernelName = kernelName.c_str();

    ze_result_t res = module->createKernel(&kernelDesc, &kernelHandle);

    EXPECT_EQ(ZE_RESULT_SUCCESS, res);

    Kernel::fromHandle(kernelHandle)->destroy();
}

using ModuleTestSupport = IsWithinProducts<IGFX_SKYLAKE, IGFX_TIGERLAKE_LP>;

HWTEST2_F(ModuleTest, givenNonPatchedTokenThenSurfaceBaseAddressIsCorrectlySet, ModuleTestSupport) {
    using RENDER_SURFACE_STATE = typename FamilyType::RENDER_SURFACE_STATE;
    ze_kernel_handle_t kernelHandle;

    ze_kernel_desc_t kernelDesc = {};
    kernelDesc.pKernelName = kernelName.c_str();

    ze_result_t res = module->createKernel(&kernelDesc, &kernelHandle);

    EXPECT_EQ(ZE_RESULT_SUCCESS, res);

    auto kernelImp = reinterpret_cast<L0::KernelImp *>(L0::Kernel::fromHandle(kernelHandle));

    void *devicePtr = nullptr;
    res = device->getDriverHandle()->allocDeviceMem(device->toHandle(),
                                                    0u,
                                                    16384u,
                                                    0u,
                                                    &devicePtr);
    ASSERT_EQ(ZE_RESULT_SUCCESS, res);

    auto gpuAlloc = device->getDriverHandle()->getSvmAllocsManager()->getSVMAllocs()->get(devicePtr)->gpuAllocations.getGraphicsAllocation(device->getRootDeviceIndex());
    ASSERT_NE(nullptr, gpuAlloc);

    uint32_t argIndex = 0u;
    kernelImp->setBufferSurfaceState(argIndex, devicePtr, gpuAlloc);

    auto argInfo = kernelImp->getImmutableData()->getDescriptor().payloadMappings.explicitArgs[argIndex].as<NEO::ArgDescPointer>();
    auto surfaceStateAddressRaw = ptrOffset(kernelImp->getSurfaceStateHeapData(), argInfo.bindful);
    auto surfaceStateAddress = reinterpret_cast<RENDER_SURFACE_STATE *>(const_cast<unsigned char *>(surfaceStateAddressRaw));
    EXPECT_EQ(reinterpret_cast<void *>(surfaceStateAddress->getSurfaceBaseAddress()), devicePtr);

    Kernel::fromHandle(kernelHandle)->destroy();

    device->getDriverHandle()->freeMem(devicePtr);
}

HWTEST_F(ModuleTest, givenKernelCreateWithIncorrectKernelNameReturnsFailure) {
    ze_kernel_handle_t kernelHandle;

    ze_kernel_desc_t kernelDesc = {};
    kernelDesc.pKernelName = "nonexistent_function";

    ze_result_t res = module->createKernel(&kernelDesc, &kernelHandle);

    EXPECT_EQ(ZE_RESULT_ERROR_INVALID_ARGUMENT, res);
}

struct ModuleSpecConstantsTests : public DeviceFixture,
                                  public ::testing::Test {
    void SetUp() override {
        DeviceFixture::SetUp();

        mockCompiler = new MockCompilerInterfaceWithSpecConstants(moduleNumSpecConstants);
        auto rootDeviceEnvironment = neoDevice->getExecutionEnvironment()->rootDeviceEnvironments[0].get();
        rootDeviceEnvironment->compilerInterface.reset(mockCompiler);

        mockTranslationUnit = new MockModuleTranslationUnit(device);
    }

    void TearDown() override {
        DeviceFixture::TearDown();
    }

    const uint32_t moduleNumSpecConstants = 4;
    ze_module_constants_t specConstants;
    std::vector<const void *> specConstantsPointerValues;

    const std::string binaryFilename = "test_kernel";
    const std::string kernelName = "test";
    MockCompilerInterfaceWithSpecConstants *mockCompiler;
    MockModuleTranslationUnit *mockTranslationUnit;
};

HWTEST_F(ModuleSpecConstantsTests, givenSpecializationConstantsSetInDescriptorTheModuleCorrectlyPassesThemToTheCompiler) {
    std::string testFile;
    retrieveBinaryKernelFilename(testFile, binaryFilename + "_", ".spv");

    size_t size = 0;
    auto src = loadDataFromFile(testFile.c_str(), size);

    ASSERT_NE(0u, size);
    ASSERT_NE(nullptr, src);

    ze_module_desc_t moduleDesc = {};
    moduleDesc.format = ZE_MODULE_FORMAT_IL_SPIRV;
    moduleDesc.pInputModule = reinterpret_cast<const uint8_t *>(src.get());
    moduleDesc.inputSize = size;

    specConstants.numConstants = mockCompiler->moduleNumSpecConstants;
    for (uint32_t i = 0; i < mockCompiler->moduleNumSpecConstants; i++) {
        specConstantsPointerValues.push_back(&mockCompiler->moduleSpecConstantsValues[i]);
    }

    specConstants.pConstantIds = mockCompiler->moduleSpecConstantsIds.data();
    specConstants.pConstantValues = specConstantsPointerValues.data();
    moduleDesc.pConstants = &specConstants;

    auto module = new Module(device, nullptr);
    module->translationUnit.reset(mockTranslationUnit);

    bool success = module->initialize(&moduleDesc, neoDevice);
    EXPECT_TRUE(success);
    module->destroy();
}

using ModuleLinkingTest = Test<DeviceFixture>;

HWTEST_F(ModuleLinkingTest, givenFailureDuringLinkingWhenCreatingModuleThenModuleInitialiationFails) {
    auto mockCompiler = new MockCompilerInterface();
    auto rootDeviceEnvironment = neoDevice->getExecutionEnvironment()->rootDeviceEnvironments[0].get();
    rootDeviceEnvironment->compilerInterface.reset(mockCompiler);

    auto mockTranslationUnit = new MockModuleTranslationUnit(device);

    auto linkerInput = std::make_unique<::WhiteBox<NEO::LinkerInput>>();
    linkerInput->valid = false;

    mockTranslationUnit->programInfo.linkerInput = std::move(linkerInput);
    uint8_t spirvData{};

    ze_module_desc_t moduleDesc = {};
    moduleDesc.format = ZE_MODULE_FORMAT_IL_SPIRV;
    moduleDesc.pInputModule = &spirvData;
    moduleDesc.inputSize = sizeof(spirvData);

    Module module(device, nullptr);
    module.translationUnit.reset(mockTranslationUnit);

    bool success = module.initialize(&moduleDesc, neoDevice);
    EXPECT_FALSE(success);
}

HWTEST_F(ModuleLinkingTest, givenRemainingUnresolvedSymbolsDuringLinkingWhenCreatingModuleThenModuleIsNotLinkedFully) {
    auto mockCompiler = new MockCompilerInterface();
    auto rootDeviceEnvironment = neoDevice->getExecutionEnvironment()->rootDeviceEnvironments[0].get();
    rootDeviceEnvironment->compilerInterface.reset(mockCompiler);

    auto mockTranslationUnit = new MockModuleTranslationUnit(device);

    auto linkerInput = std::make_unique<::WhiteBox<NEO::LinkerInput>>();

    NEO::LinkerInput::RelocationInfo relocation;
    relocation.symbolName = "unresolved";
    linkerInput->dataRelocations.push_back(relocation);
    linkerInput->traits.requiresPatchingOfGlobalVariablesBuffer = true;

    mockTranslationUnit->programInfo.linkerInput = std::move(linkerInput);
    uint8_t spirvData{};

    ze_module_desc_t moduleDesc = {};
    moduleDesc.format = ZE_MODULE_FORMAT_IL_SPIRV;
    moduleDesc.pInputModule = &spirvData;
    moduleDesc.inputSize = sizeof(spirvData);

    Module module(device, nullptr);
    module.translationUnit.reset(mockTranslationUnit);

    bool success = module.initialize(&moduleDesc, neoDevice);
    EXPECT_TRUE(success);
    EXPECT_FALSE(module.isFullyLinked);
}
HWTEST_F(ModuleLinkingTest, givenNotFullyLinkedModuleWhenCreatingKernelThenErrorIsReturned) {
    Module module(device, nullptr);
    module.isFullyLinked = false;

    auto retVal = module.createKernel(nullptr, nullptr);

    EXPECT_EQ(ZE_RESULT_ERROR_MODULE_BUILD_FAILURE, retVal);
}
struct ModuleDynamicLinkTests : public Test<ModuleFixture> {
    void SetUp() override {
        Test<ModuleFixture>::SetUp();
        module0 = std::make_unique<Module>(device, nullptr);
        module1 = std::make_unique<Module>(device, nullptr);
    }
    std::unique_ptr<Module> module0;
    std::unique_ptr<Module> module1;
};

TEST_F(ModuleDynamicLinkTests, givenCallToDynamicLinkOnModulesWithoutUnresolvedSymbolsThenSuccessIsReturned) {
    std::vector<ze_module_handle_t> hModules = {module0->toHandle(), module1->toHandle()};
    ze_result_t res = module0->performDynamicLink(2, hModules.data(), nullptr);
    EXPECT_EQ(ZE_RESULT_SUCCESS, res);
}

TEST_F(ModuleDynamicLinkTests, givenModuleWithUnresolvedSymbolsNotPresentInOtherModulesWhenDynamicLinkThenLinkFailureIsReturned) {

    NEO::Linker::RelocationInfo unresolvedRelocation;
    unresolvedRelocation.symbolName = "unresolved";

    module0->unresolvedExternalsInfo.push_back({unresolvedRelocation});

    std::vector<ze_module_handle_t> hModules = {module0->toHandle(), module1->toHandle()};
    ze_result_t res = module0->performDynamicLink(2, hModules.data(), nullptr);
    EXPECT_EQ(ZE_RESULT_ERROR_MODULE_LINK_FAILURE, res);
}
TEST_F(ModuleDynamicLinkTests, whenModuleIsAlreadyLinkedThenThereIsNoSymbolsVerification) {

    NEO::Linker::RelocationInfo unresolvedRelocation;
    unresolvedRelocation.symbolName = "unresolved";

    module0->unresolvedExternalsInfo.push_back({unresolvedRelocation});
    module0->isFullyLinked = true;

    std::vector<ze_module_handle_t> hModules = {module0->toHandle(), module1->toHandle()};
    ze_result_t res = module0->performDynamicLink(2, hModules.data(), nullptr);
    EXPECT_EQ(ZE_RESULT_SUCCESS, res);
}

TEST_F(ModuleDynamicLinkTests, givenModuleWithUnresolvedSymbolWhenTheOtherModuleDefinesTheSymbolThenTheSegmentIsPatched) {

    uint64_t gpuAddress = 0x12345;
    uint32_t offset = 0x20;

    NEO::Linker::RelocationInfo unresolvedRelocation;
    unresolvedRelocation.symbolName = "unresolved";
    unresolvedRelocation.offset = offset;
    unresolvedRelocation.type = NEO::Linker::RelocationInfo::Type::Address;
    NEO::Linker::UnresolvedExternal unresolvedExternal;
    unresolvedExternal.unresolvedRelocation = unresolvedRelocation;

    NEO::SymbolInfo symbolInfo{};
    NEO::Linker::RelocatedSymbol relocatedSymbol{symbolInfo, gpuAddress};

    char kernelHeap[MemoryConstants::pageSize] = {};

    auto kernelInfo = std::make_unique<NEO::KernelInfo>();
    kernelInfo->heapInfo.pKernelHeap = kernelHeap;
    kernelInfo->heapInfo.KernelHeapSize = MemoryConstants::pageSize;
    module0->getTranslationUnit()->programInfo.kernelInfos.push_back(kernelInfo.release());

    auto linkerInput = std::make_unique<::WhiteBox<NEO::LinkerInput>>();
    linkerInput->traits.requiresPatchingOfInstructionSegments = true;

    module0->getTranslationUnit()->programInfo.linkerInput = std::move(linkerInput);
    module0->unresolvedExternalsInfo.push_back({unresolvedRelocation});
    module0->unresolvedExternalsInfo[0].instructionsSegmentId = 0u;

    auto kernelImmData = std::make_unique<WhiteBox<::L0::KernelImmutableData>>(device);
    kernelImmData->isaGraphicsAllocation.reset(neoDevice->getMemoryManager()->allocateGraphicsMemoryWithProperties(
        {device->getRootDeviceIndex(), MemoryConstants::pageSize, NEO::GraphicsAllocation::AllocationType::KERNEL_ISA, neoDevice->getDeviceBitfield()}));

    auto isaPtr = kernelImmData->getIsaGraphicsAllocation()->getUnderlyingBuffer();

    module0->kernelImmDatas.push_back(std::move(kernelImmData));

    module1->symbols[unresolvedRelocation.symbolName] = relocatedSymbol;

    std::vector<ze_module_handle_t> hModules = {module0->toHandle(), module1->toHandle()};
    ze_result_t res = module0->performDynamicLink(2, hModules.data(), nullptr);
    EXPECT_EQ(ZE_RESULT_SUCCESS, res);

    EXPECT_EQ(gpuAddress, *reinterpret_cast<uint64_t *>(ptrOffset(isaPtr, offset)));
}

class MultiDeviceModuleSetArgBufferTest : public MultiDeviceModuleFixture, public ::testing::Test {
  public:
    void SetUp() override {
        MultiDeviceModuleFixture::SetUp();
    }

    void TearDown() override {
        MultiDeviceModuleFixture::TearDown();
    }

    void createKernelAndAllocMemory(uint32_t rootDeviceIndex, void **ptr, ze_kernel_handle_t *kernelHandle) {
        ze_kernel_desc_t kernelDesc = {};
        kernelDesc.pKernelName = kernelName.c_str();
        ze_result_t res = modules[rootDeviceIndex].get()->createKernel(&kernelDesc, kernelHandle);
        EXPECT_EQ(ZE_RESULT_SUCCESS, res);

        res = driverHandle->allocHostMem(0u, 4096u, rootDeviceIndex, ptr);
        EXPECT_EQ(ZE_RESULT_SUCCESS, res);
    }
};

HWTEST_F(MultiDeviceModuleSetArgBufferTest,
         givenCallsToSetArgBufferThenAllocationIsSetForCorrectDevice) {

    for (uint32_t rootDeviceIndex = 0; rootDeviceIndex < numRootDevices; rootDeviceIndex++) {
        createModuleFromBinary(rootDeviceIndex);

        ze_kernel_handle_t kernelHandle;
        void *ptr = nullptr;
        createKernelAndAllocMemory(rootDeviceIndex, &ptr, &kernelHandle);

        L0::KernelImp *kernel = reinterpret_cast<L0::KernelImp *>(Kernel::fromHandle(kernelHandle));
        kernel->setArgBuffer(0, sizeof(ptr), &ptr);

        for (auto alloc : kernel->getResidencyContainer()) {
            if (alloc && alloc->getGpuAddress() == reinterpret_cast<uint64_t>(ptr)) {
                EXPECT_EQ(rootDeviceIndex, alloc->getRootDeviceIndex());
            }
        }

        driverHandle->freeMem(ptr);
        Kernel::fromHandle(kernelHandle)->destroy();
    }
}

using ContextModuleCreateTest = Test<ContextFixture>;

HWTEST_F(ContextModuleCreateTest, givenCallToCreateModuleThenModuleIsReturned) {
    std::string testFile;
    retrieveBinaryKernelFilename(testFile, "test_kernel_", ".bin");

    size_t size = 0;
    auto src = loadDataFromFile(testFile.c_str(), size);

    ASSERT_NE(0u, size);
    ASSERT_NE(nullptr, src);

    ze_module_desc_t moduleDesc = {};
    moduleDesc.format = ZE_MODULE_FORMAT_NATIVE;
    moduleDesc.pInputModule = reinterpret_cast<const uint8_t *>(src.get());
    moduleDesc.inputSize = size;

    ze_module_handle_t hModule;
    ze_device_handle_t hDevice = device->toHandle();
    ze_result_t res = context->createModule(hDevice, &moduleDesc, &hModule, nullptr);
    EXPECT_EQ(ZE_RESULT_SUCCESS, res);

    L0::Module *pModule = L0::Module::fromHandle(hModule);
    res = pModule->destroy();
    EXPECT_EQ(ZE_RESULT_SUCCESS, res);
}

using ModuleTranslationUnitTest = Test<DeviceFixture>;

HWTEST_F(ModuleTranslationUnitTest, WhenCreatingFromNativeBinaryThenSetsUpRequiredTargetProductProperly) {
    ZebinTestData::ValidEmptyProgram emptyProgram;
    auto hwInfo = device->getNEODevice()->getHardwareInfo();

    emptyProgram.elfHeader->machine = hwInfo.platform.eProductFamily;
    L0::ModuleTranslationUnit moduleTuValid(this->device);
    bool success = moduleTuValid.createFromNativeBinary(reinterpret_cast<const char *>(emptyProgram.storage.data()), emptyProgram.storage.size());
    EXPECT_TRUE(success);

    emptyProgram.elfHeader->machine = hwInfo.platform.eProductFamily;
    ++emptyProgram.elfHeader->machine;
    L0::ModuleTranslationUnit moduleTuInvalid(this->device);
    success = moduleTuInvalid.createFromNativeBinary(reinterpret_cast<const char *>(emptyProgram.storage.data()), emptyProgram.storage.size());
    EXPECT_FALSE(success);
}

} // namespace ult
} // namespace L0
