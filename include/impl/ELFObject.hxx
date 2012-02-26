/*
 * Copyright 2011, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ELF_OBJECT_HXX
#define ELF_OBJECT_HXX

#include "ELFHeader.h"
#include "ELFReloc.h"
#include "ELFSection.h"
#include "ELFSectionHeaderTable.h"
#include "StubLayout.h"
#include "GOT.h"
#include "ELF.h"

#include <llvm/ADT/SmallVector.h>

#include "utils/rsl_assert.h"

template <unsigned Bitwidth>
template <typename Archiver>
inline ELFObject<Bitwidth> *
ELFObject<Bitwidth>::read(Archiver &AR) {
  llvm::OwningPtr<ELFObjectTy> object(new ELFObjectTy());

  // Read header
  object->header.reset(ELFHeaderTy::read(AR));
  if (!object->header) {
    return 0;
  }

  // Read section table
  object->shtab.reset(ELFSectionHeaderTableTy::read(AR, object.get()));
  if (!object->shtab) {
    return 0;
  }

  // Read each section
  llvm::SmallVector<size_t, 4> progbits_ndx;
  for (size_t i = 0; i < object->header->getSectionHeaderNum(); ++i) {
    if ((*object->shtab)[i]->getType() == SHT_PROGBITS) {
      object->stab.push_back(NULL);
      progbits_ndx.push_back(i);
    } else {
      llvm::OwningPtr<ELFSectionTy> sec(
        ELFSectionTy::read(AR, object.get(), (*object->shtab)[i]));
      object->stab.push_back(sec.take());
    }
  }

  object->shtab->buildNameMap();
  ELFSectionSymTabTy *symtab =
    static_cast<ELFSectionSymTabTy *>(object->getSectionByName(".symtab"));
  rsl_assert(symtab && "Symtab is required.");
  symtab->buildNameMap();

  for (size_t i = 0; i < progbits_ndx.size(); ++i) {
    size_t index = progbits_ndx[i];

    llvm::OwningPtr<ELFSectionTy> sec(
      ELFSectionTy::read(AR, object.get(), (*object->shtab)[index]));
    object->stab[index] = sec.take();
  }

  return object.take();
}

template <unsigned Bitwidth>
inline char const *ELFObject<Bitwidth>::getSectionName(size_t i) const {
  ELFSectionTy const *sec = stab[header->getStringSectionIndex()];

  if (sec) {
    ELFSectionStrTabTy const &st =
      static_cast<ELFSectionStrTabTy const &>(*sec);
    return st[i];
  }

  return NULL;
}

template <unsigned Bitwidth>
inline ELFSection<Bitwidth> const *
ELFObject<Bitwidth>::getSectionByIndex(size_t i) const {
  return stab[i];
}

template <unsigned Bitwidth>
inline ELFSection<Bitwidth> *
ELFObject<Bitwidth>::getSectionByIndex(size_t i) {
  return stab[i];
}

template <unsigned Bitwidth>
inline ELFSection<Bitwidth> const *
ELFObject<Bitwidth>::getSectionByName(std::string const &str) const {
  size_t idx = getSectionHeaderTable()->getByName(str)->getIndex();
  return stab[idx];
}

template <unsigned Bitwidth>
inline ELFSection<Bitwidth> *
ELFObject<Bitwidth>::getSectionByName(std::string const &str) {
  ELFObjectTy const *const_this = this;
  ELFSectionTy const *sptr = const_this->getSectionByName(str);
  // Const cast for the same API's const and non-const versions.
  return const_cast<ELFSectionTy *>(sptr);
}


template <unsigned Bitwidth>
inline void ELFObject<Bitwidth>::
relocateARM(void *(*find_sym)(void *context, char const *name),
            void *context,
            ELFSectionRelTableTy *reltab,
            ELFSectionProgBitsTy *text) {
  // FIXME: Should be implement in independent files.
  rsl_assert(Bitwidth == 32 && "ARM only have 32 bits.");

  ELFSectionSymTabTy *symtab =
    static_cast<ELFSectionSymTabTy *>(getSectionByName(".symtab"));
  rsl_assert(symtab && "Symtab is required.");

  for (size_t i = 0; i < reltab->size(); ++i) {
    // FIXME: Can not implement here, use Fixup!
    ELFRelocTy *rel = (*reltab)[i];
    ELFSymbolTy *sym = (*symtab)[rel->getSymTabIndex()];

    // FIXME: May be not uint32_t *.
    typedef int32_t Inst_t;
    Inst_t *inst = (Inst_t *)&(*text)[rel->getOffset()];
    Inst_t P = (Inst_t)(int64_t)inst;
    Inst_t A = 0;
    Inst_t S = (Inst_t)(int64_t)sym->getAddress(EM_ARM);

    switch (rel->getType()) {
    default:
      rsl_assert(0 && "Not implemented relocation type.");
      break;

    case R_ARM_ABS32:
      {
        A = *inst;
        *inst = (S+A);
      }
      break;

      // FIXME: Predefine relocation codes.
    case R_ARM_CALL:
      {
#define SIGN_EXTEND(x, l) (((x)^(1<<((l)-1)))-(1<<(l-1)))
        A = (Inst_t)(int64_t)SIGN_EXTEND(*inst & 0xFFFFFF, 24);
#undef SIGN_EXTEND

        void *callee_addr = sym->getAddress(EM_ARM);

        switch (sym->getType()) {
        default:
          rsl_assert(0 && "Wrong type for R_ARM_CALL relocation.");
          abort();
          break;

        case STT_FUNC:
          // NOTE: Callee function is in the object file, but it may be
          // in different PROGBITS section (which may be far call).

          if (callee_addr == 0) {
            rsl_assert(0 && "We should get function address at previous "
                   "sym->getAddress(EM_ARM) function call.");
            abort();
          }
          break;

        case STT_NOTYPE:
          // NOTE: Callee function is an external function.  Call find_sym
          // if it has not resolved yet.

          if (callee_addr == 0) {
            callee_addr = find_sym(context, sym->getName());
            if (!callee_addr) {
              missingSymbols = true;
            }
            sym->setAddress(callee_addr);
          }
          break;
        }

        // Get the stub for this function
        StubLayout *stub_layout = text->getStubLayout();

        if (!stub_layout) {
          llvm::errs() << "unable to get stub layout." << "\n";
          abort();
        }

        void *stub = stub_layout->allocateStub(callee_addr);

        if (!stub) {
          llvm::errs() << "unable to allocate stub." << "\n";
          abort();
        }

        //LOGI("Function %s: using stub %p\n", sym->getName(), stub);
        S = (uint32_t)(uintptr_t)stub;

        // Relocate the R_ARM_CALL relocation type
        uint32_t result = (S >> 2) - (P >> 2) + A;

        if (result > 0x007fffff && result < 0xff800000) {
          rsl_assert(0 && "Stub is still too far");
          abort();
        }

        *inst = ((result) & 0x00FFFFFF) | (*inst & 0xFF000000);
      }
      break;
    case R_ARM_MOVT_ABS:
    case R_ARM_MOVW_ABS_NC:
      {
        if (S==0 && sym->getType() == STT_NOTYPE)
        {
          void *ext_sym = find_sym(context, sym->getName());
          if (!ext_sym) {
            missingSymbols = true;
          }
          S = (Inst_t)(uintptr_t)ext_sym;
          sym->setAddress(ext_sym);
        }
        if (rel->getType() == R_ARM_MOVT_ABS) {
          S >>= 16;
        }

        // No need sign extend.
        A = ((*inst & 0xF0000) >> 4) | (*inst & 0xFFF);
        uint32_t result = (S+A);
        *inst = (((result) & 0xF000) << 4) |
          ((result) & 0xFFF) |
          (*inst & 0xFFF0F000);
      }
      break;
    }
    //llvm::errs() << "S:     " << (void *)S << '\n';
    //llvm::errs() << "A:     " << (void *)A << '\n';
    //llvm::errs() << "P:     " << (void *)P << '\n';
    //llvm::errs() << "S+A:   " << (void *)(S+A) << '\n';
    //llvm::errs() << "S+A-P: " << (void *)(S+A-P) << '\n';
  }
}

template <unsigned Bitwidth>
inline void ELFObject<Bitwidth>::
relocateX86_64(void *(*find_sym)(void *context, char const *name),
               void *context,
               ELFSectionRelTableTy *reltab,
               ELFSectionProgBitsTy *text) {
  rsl_assert(Bitwidth == 64 && "Only support X86_64.");

  ELFSectionSymTabTy *symtab =
    static_cast<ELFSectionSymTabTy *>(getSectionByName(".symtab"));
  rsl_assert(symtab && "Symtab is required.");

  for (size_t i = 0; i < reltab->size(); ++i) {
    // FIXME: Can not implement here, use Fixup!
    ELFRelocTy *rel = (*reltab)[i];
    ELFSymbolTy *sym = (*symtab)[rel->getSymTabIndex()];

    //typedef uint64_t Inst_t;
    typedef int32_t Inst_t;
    Inst_t *inst = (Inst_t *)&(*text)[rel->getOffset()];
    Inst_t P = (Inst_t)(int64_t)inst;
    Inst_t A = (Inst_t)(int64_t)rel->getAddend();
    Inst_t S = (Inst_t)(int64_t)sym->getAddress(EM_X86_64);

    if (S == 0) {
      S = (Inst_t)(int64_t)find_sym(context, sym->getName());
      if (!S) {
        missingSymbols = true;
      }
      sym->setAddress((void *)S);
    }

    switch (rel->getType()) {
      default:
        rsl_assert(0 && "Not implemented relocation type.");
        break;

      // FIXME: XXX: R_X86_64_64 is 64 bit, there is a big problem here.
      case 1: // R_X86_64_64
        *inst = (S+A);
        break;

      case 2: // R_X86_64_PC32
        *inst = (S+A-P);
        break;

      case 10: // R_X86_64_32
      case 11: // R_X86_64_32S
        *inst = (S+A);
        break;
    }
  }
}

template <unsigned Bitwidth>
inline void ELFObject<Bitwidth>::
relocateX86_32(void *(*find_sym)(void *context, char const *name),
               void *context,
               ELFSectionRelTableTy *reltab,
               ELFSectionProgBitsTy *text) {
  rsl_assert(Bitwidth == 32 && "Only support X86.");

  ELFSectionSymTabTy *symtab =
    static_cast<ELFSectionSymTabTy *>(getSectionByName(".symtab"));
  rsl_assert(symtab && "Symtab is required.");

  for (size_t i = 0; i < reltab->size(); ++i) {
    // FIXME: Can not implement here, use Fixup!
    ELFRelocTy *rel = (*reltab)[i];
    ELFSymbolTy *sym = (*symtab)[rel->getSymTabIndex()];

    //typedef uint64_t Inst_t;
    typedef int32_t Inst_t;
    Inst_t *inst = (Inst_t *)&(*text)[rel->getOffset()];
    Inst_t P = (Inst_t)(uintptr_t)inst;
    Inst_t A = (Inst_t)(uintptr_t)*inst;
    Inst_t S = (Inst_t)(uintptr_t)sym->getAddress(EM_386);

    if (S == 0) {
      S = (Inst_t)(uintptr_t)find_sym(context, sym->getName());
      if (!S) {
        missingSymbols = true;
      }
      sym->setAddress((void *)S);
    }

    switch (rel->getType()) {
    default:
      rsl_assert(0 && "Not implemented relocation type.");
      break;

    case R_386_PC32:
      *inst = (S+A-P);
      break;

    case R_386_32:
      *inst = (S+A);
      break;
    }
  }
}

template <unsigned Bitwidth>
inline void ELFObject<Bitwidth>::
relocateMIPS(void *(*find_sym)(void *context, char const *name),
             void *context,
             ELFSectionRelTableTy *reltab,
             ELFSectionProgBitsTy *text) {
  rsl_assert(Bitwidth == 32 && "Only support 32-bit MIPS.");

  ELFSectionSymTabTy *symtab =
    static_cast<ELFSectionSymTabTy *>(getSectionByName(".symtab"));
  rsl_assert(symtab && "Symtab is required.");

  for (size_t i = 0; i < reltab->size(); ++i) {
    // FIXME: Can not implement here, use Fixup!
    ELFRelocTy *rel = (*reltab)[i];
    ELFSymbolTy *sym = (*symtab)[rel->getSymTabIndex()];

    typedef int32_t Inst_t;
    Inst_t *inst = (Inst_t *)&(*text)[rel->getOffset()];
    Inst_t P = (Inst_t)(uintptr_t)inst;
    Inst_t A = (Inst_t)(uintptr_t)*inst;
    Inst_t S = (Inst_t)(uintptr_t)sym->getAddress(EM_MIPS);

    bool need_stub = false;

    if (S == 0 && strcmp (sym->getName(), "_gp_disp") != 0) {
      need_stub = true;
      S = (Inst_t)(uintptr_t)find_sym(context, sym->getName());
      if (!S) {
        missingSymbols = true;
      }
      sym->setAddress((void *)S);
    }

    switch (rel->getType()) {
    default:
      rsl_assert(0 && "Not implemented relocation type.");
      break;

    case R_MIPS_NONE:
    case R_MIPS_JALR: // ignore this
      break;

    case R_MIPS_16:
      *inst &= 0xFFFF0000;
      A = A & 0xFFFF;
      A = S + (short)A;
      rsl_assert(A >= -32768 && A <= 32767 && "R_MIPS_16 overflow.");
      *inst |= (A & 0xFFFF);
      break;

    case R_MIPS_32:
      *inst = S + A;
      break;

    case R_MIPS_26:
      *inst &= 0xFC000000;
      if (need_stub == false) {
        A = (A & 0x3FFFFFF) << 2;
        if (sym->getBindingAttribute() == STB_LOCAL) { // local binding
          A |= ((P + 4) & 0xF0000000);
          A += S;
          *inst |= ((A >> 2) & 0x3FFFFFF);
        }
        else { // external binding
          if (A & 0x08000000) // Sign extend from bit 27
            A |= 0xF0000000;
          A += S;
          *inst |= ((A >> 2) & 0x3FFFFFF);
          if (((P + 4) >> 28) != (A >> 28)) { // far local call
            void *stub = text->getStubLayout()->allocateStub((void *)A);
            rsl_assert(stub && "cannot allocate stub.");
            sym->setAddress(stub);
            S = (int32_t)(intptr_t)stub;
            *inst |= ((S >> 2) & 0x3FFFFFF);
            rsl_assert(((P + 4) >> 28) == (S >> 28) && "stub is too far.");
          }
        }
      }
      else { // shared-library call
        A = (A & 0x3FFFFFF) << 2;
        rsl_assert(A == 0 && "R_MIPS_26 addend is not zero.");
        void *stub = text->getStubLayout()->allocateStub((void *)S);
        rsl_assert(stub && "cannot allocate stub.");
        sym->setAddress(stub);
        S = (int32_t)(intptr_t)stub;
        *inst |= ((S >> 2) & 0x3FFFFFF);
        rsl_assert(((P + 4) >> 28) == (S >> 28) && "stub is too far.");
      }
      break;

    case R_MIPS_HI16:
      *inst &= 0xFFFF0000;
      A = (A & 0xFFFF) << 16;
      // Find the nearest LO16 relocation type after this entry
      for (size_t j = i + 1; j < reltab->size(); j++) {
        ELFRelocTy *this_rel = (*reltab)[j];
        ELFSymbolTy *this_sym = (*symtab)[this_rel->getSymTabIndex()];
        if (this_rel->getType() == R_MIPS_LO16 && this_sym == sym) {
          Inst_t *this_inst = (Inst_t *)&(*text)[this_rel->getOffset()];
          Inst_t this_A = (Inst_t)(uintptr_t)*this_inst;
          this_A = this_A & 0xFFFF;
          A += (short)this_A;
          break;
        }
      }
      if (strcmp (sym->getName(), "_gp_disp") == 0) {
          S = (int)(intptr_t)got_address() + GP_OFFSET - (int)P;
          sym->setAddress((void *)S);
      }
      *inst |= (((S + A + (int)0x8000) >> 16) & 0xFFFF);
      break;

    case R_MIPS_LO16:
      *inst &= 0xFFFF0000;
      A = A & 0xFFFF;
      if (strcmp (sym->getName(), "_gp_disp") == 0) {
          S = (Inst_t)(intptr_t)sym->getAddress(EM_MIPS);
      }
      *inst |= ((S + A) & 0xFFFF);
      break;

    case R_MIPS_GOT16:
    case R_MIPS_CALL16:
      {
        *inst &= 0xFFFF0000;
        A = A & 0xFFFF;
        if (rel->getType() == R_MIPS_GOT16) {
          if (sym->getBindingAttribute() == STB_LOCAL) {
            A <<= 16;

            // Find the nearest LO16 relocation type after this entry
            for (size_t j = i + 1; j < reltab->size(); j++) {
              ELFRelocTy *this_rel = (*reltab)[j];
              ELFSymbolTy *this_sym = (*symtab)[this_rel->getSymTabIndex()];
              if (this_rel->getType() == R_MIPS_LO16 && this_sym == sym) {
                Inst_t *this_inst = (Inst_t *)&(*text)[this_rel->getOffset()];
                Inst_t this_A = (Inst_t)(uintptr_t)*this_inst;
                this_A = this_A & 0xFFFF;
                A += (short)this_A;
                break;
              }
            }
          }
          else {
            rsl_assert(A == 0 && "R_MIPS_GOT16 addend is not 0.");
          }
        }
        else { // R_MIPS_CALL16
          rsl_assert(A == 0 && "R_MIPS_CALL16 addend is not 0.");
        }
        int got_index = search_got((int)rel->getSymTabIndex(), (void *)(S + A),
                                   sym->getBindingAttribute());
        int got_offset = (got_index << 2) - GP_OFFSET;
        *inst |= (got_offset & 0xFFFF);
      }
      break;

    case R_MIPS_GPREL32:
      *inst = A + S - ((int)(intptr_t)got_address() + GP_OFFSET);
      break;
    }
  }
}


// TODO: Refactor all relocations.
template <unsigned Bitwidth>
inline void ELFObject<Bitwidth>::
relocate(void *(*find_sym)(void *context, char const *name), void *context) {
  // Init SHNCommonDataSize.
  // Need refactoring
  size_t SHNCommonDataSize = 0;

  ELFSectionSymTabTy *symtab =
    static_cast<ELFSectionSymTabTy *>(getSectionByName(".symtab"));
  rsl_assert(symtab && "Symtab is required.");

  for (size_t i = 0; i < symtab->size(); ++i) {
    ELFSymbolTy *sym = (*symtab)[i];

    if (sym->getType() != STT_OBJECT) {
      continue;
    }

    size_t idx = (size_t)sym->getSectionIndex();
    switch (idx) {
    default:
      if ((*shtab)[idx]->getType() == SHT_NOBITS) {
        // FIXME(logan): This is a workaround for .lcomm directives
        // bug of LLVM ARM MC code generator.  Remove this when the
        // LLVM bug is fixed.

        size_t align = 16;
        SHNCommonDataSize += (size_t)sym->getSize() + align;
      }
      break;

    case SHN_COMMON:
      {
        size_t align = (size_t)sym->getValue();
        SHNCommonDataSize += (size_t)sym->getSize() + align;
      }
      break;

    case SHN_ABS:
    case SHN_UNDEF:
    case SHN_XINDEX:
      break;
    }
  }
  if (!initSHNCommonDataSize(SHNCommonDataSize)) {
    rsl_assert("Allocate memory for common variable fail!");
  }

  for (size_t i = 0; i < stab.size(); ++i) {
    ELFSectionHeaderTy *sh = (*shtab)[i];
    if (sh->getType() != SHT_REL && sh->getType() != SHT_RELA) {
      continue;
    }
    ELFSectionRelTableTy *reltab =
      static_cast<ELFSectionRelTableTy *>(stab[i]);
    rsl_assert(reltab && "Relocation section can't be NULL.");

    const char *reltab_name = sh->getName();
    const char *need_rel_name;
    if (sh->getType() == SHT_REL) {
      need_rel_name = reltab_name + 4;
      // ".rel.xxxx"
      //      ^ start from here.
    } else {
      need_rel_name = reltab_name + 5;
    }

    ELFSectionProgBitsTy *need_rel =
      static_cast<ELFSectionProgBitsTy *>(getSectionByName(need_rel_name));
    rsl_assert(need_rel && "Need be relocated section can't be NULL.");

    switch (getHeader()->getMachine()) {
      case EM_ARM:
        relocateARM(find_sym, context, reltab, need_rel);
        break;
      case EM_386:
        relocateX86_32(find_sym, context, reltab, need_rel);
        break;
      case EM_X86_64:
        relocateX86_64(find_sym, context, reltab, need_rel);
        break;
      case EM_MIPS:
        relocateMIPS(find_sym, context, reltab, need_rel);
        break;

      default:
        rsl_assert(0 && "Only support ARM, MIPS, X86, and X86_64 relocation.");
        break;
    }
  }

  for (size_t i = 0; i < stab.size(); ++i) {
    ELFSectionHeaderTy *sh = (*shtab)[i];
    if (sh->getType() == SHT_PROGBITS || sh->getType() == SHT_NOBITS) {
      if (stab[i]) {
        static_cast<ELFSectionBitsTy *>(stab[i])->protect();
      }
    }
  }
}

template <unsigned Bitwidth>
inline void ELFObject<Bitwidth>::print() const {
  header->print();
  shtab->print();

  for (size_t i = 0; i < stab.size(); ++i) {
    ELFSectionTy *sec = stab[i];
    if (sec) {
      sec->print();
    }
  }
}

#endif // ELF_OBJECT_HXX
