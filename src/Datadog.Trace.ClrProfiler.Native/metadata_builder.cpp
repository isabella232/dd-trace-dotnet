﻿#include <fstream>
#include <string>

#include "clr_helpers.h"
#include "macros.h"
#include "metadata_builder.h"

namespace trace {

MetadataBuilder::MetadataBuilder(
    ModuleMetadata& metadata, const mdModule module,
    ComPtr<IMetaDataImport2> metadata_import,
    ComPtr<IMetaDataEmit> metadata_emit,
    ComPtr<IMetaDataAssemblyImport> assembly_import,
    ComPtr<IMetaDataAssemblyEmit> assembly_emit)
    : metadata_(metadata),
      module_(module),
      metadata_import_(std::move(metadata_import)),
      metadata_emit_(std::move(metadata_emit)),
      assembly_import_(std::move(assembly_import)),
      assembly_emit_(std::move(assembly_emit)) {
  auto systemasm = mdAssemblyRefNil;
  const std::vector<LPCWSTR> possible_assembly_names = {
      L"System.Runtime", L"mscorlib", L"netstandard"};
  for (auto& assembly_name : possible_assembly_names) {
    systemasm = FindAssemblyRef(assembly_import_, assembly_name);
    if (systemasm != mdAssemblyRefNil) {
      break;
    }
  }

  const std::vector<LPCWSTR> type_names = {L"System.Object",
                                           L"System.Exception"};
  for (auto& type_name : type_names) {
    mdTypeRef typeref = mdTypeRefNil;
    auto hr =
        metadata_emit_->DefineTypeRefByName(systemasm, type_name, &typeref);
    if (!FAILED(hr)) {
      metadata_.type_refs[type_name] = typeref;
    }
  }
}

HRESULT MetadataBuilder::EmitAssemblyRef(
    const trace::AssemblyReference& assembly_ref) const {
  ASSEMBLYMETADATA assembly_metadata{};
  assembly_metadata.usMajorVersion = assembly_ref.version.major;
  assembly_metadata.usMinorVersion = assembly_ref.version.minor;
  assembly_metadata.usBuildNumber = assembly_ref.version.build;
  assembly_metadata.usRevisionNumber = assembly_ref.version.revision;
  if (assembly_ref.locale == L"neutral") {
    assembly_metadata.szLocale = nullptr;
    assembly_metadata.cbLocale = 0;
  } else {
    assembly_metadata.szLocale =
        const_cast<wchar_t*>(assembly_ref.locale.data());
    assembly_metadata.cbLocale = (DWORD)(assembly_ref.locale.size());
  }

  LOG_APPEND("[MetadataBuilder::EmitAssemblyRef] added assembly ref to "
             << assembly_ref.str());

  DWORD public_key_size = 8;
  if (assembly_ref.public_key == trace::PublicKey()) {
    public_key_size = 0;
  }

  mdAssemblyRef assembly_ref_out;
  const HRESULT hr = assembly_emit_->DefineAssemblyRef(
      &assembly_ref.public_key.data[0], public_key_size,
      assembly_ref.name.c_str(), &assembly_metadata,
      // hash blob
      nullptr,
      // cb of hash blob
      0,
      // flags
      0, &assembly_ref_out);

  LOG_IFFAILEDRET(hr, L"DefineAssemblyRef failed");
  return S_OK;
}

HRESULT MetadataBuilder::FindTypeReference(const TypeReference& type_reference,
                                           mdTypeRef& type_ref_out) const {
  const auto& cache_key = type_reference.get_type_cache_key();
  mdTypeRef type_ref = mdTypeRefNil;

  if (metadata_.TryGetWrapperParentTypeRef(cache_key, type_ref)) {
    // this type was already resolved
    type_ref_out = type_ref;
    return S_OK;
  }

  HRESULT hr;
  type_ref = mdTypeRefNil;

  const LPCWSTR wrapper_type_name = type_reference.type_name.c_str();

  if (metadata_.assemblyName == type_reference.assembly.name) {
    // type is defined in this assembly
    hr = metadata_emit_->DefineTypeRefByName(module_, wrapper_type_name,
                                             &type_ref);
  } else {
    // type is defined in another assembly,
    // find a reference to the assembly where type lives
    const auto assembly_ref =
        FindAssemblyRef(assembly_import_, type_reference.assembly.name);
    if (assembly_ref == mdAssemblyRefNil) {
      // TODO: emit assembly reference if not found?
      LOG_APPEND("Assembly reference for " << type_reference.assembly.name
                                           << " not found.");
      return E_FAIL;
    }

    // search for an existing reference to the type
    hr = metadata_import_->FindTypeRef(assembly_ref, wrapper_type_name,
                                       &type_ref);

    if (hr == HRESULT(0x80131130) /* record not found on lookup */) {
      // if typeRef not found, create a new one by emitting a metadata token
      hr = metadata_emit_->DefineTypeRefByName(assembly_ref, wrapper_type_name,
                                               &type_ref);
    }
  }

  RETURN_IF_FAILED(hr);

  // cache the typeRef in case we need it again
  metadata_.SetWrapperParentTypeRef(cache_key, type_ref);
  type_ref_out = type_ref;
  return S_OK;
}

HRESULT MetadataBuilder::StoreMethodAdvice(
    const MethodAdvice& method_advice) const {
  auto hr = StoreMethodReference(method_advice.OnMethodEnterReference());
  if (FAILED(hr)) {
    return hr;
  }

  hr = StoreMethodReference(method_advice.OnMethodExitReference());
  if (FAILED(hr)) {
    return hr;
  }

  return S_OK;
}

HRESULT MetadataBuilder::StoreMethodReference(
    const MethodReference& method_reference) const {
  const auto& cache_key = method_reference.get_method_cache_key();
  mdMemberRef member_ref = mdMemberRefNil;

  if (metadata_.TryGetWrapperMemberRef(cache_key, member_ref)) {
    // this member was already resolved
    return S_OK;
  }

  auto hr = EmitAssemblyRef(method_reference.type_reference.assembly);
  if (FAILED(hr)) {
    return hr;
  }

  mdTypeRef type_ref = mdTypeRefNil;
  hr = FindTypeReference(method_reference.type_reference, type_ref);
  RETURN_IF_FAILED(hr);

  const auto wrapper_method_name = method_reference.method_name.c_str();
  member_ref = mdMemberRefNil;

  hr = metadata_import_->FindMemberRef(
      type_ref, wrapper_method_name,
      method_reference.method_signature.data.data(),
      (DWORD)(method_reference.method_signature.data.size()), &member_ref);

  if (hr == HRESULT(0x80131130) /* record not found on lookup */) {
    // if memberRef not found, create it by emitting a metadata token
    hr = metadata_emit_->DefineMemberRef(
        type_ref, wrapper_method_name,
        method_reference.method_signature.data.data(),
        (DWORD)(method_reference.method_signature.data.size()), &member_ref);
  }

  LOG_APPEND(
      L"[MetadataBuilder::StoreMethodReference] added method reference to "
      << cache_key);

  RETURN_IF_FAILED(hr);

  metadata_.SetWrapperMemberRef(cache_key, member_ref);
  return S_OK;
}

}  // namespace trace
