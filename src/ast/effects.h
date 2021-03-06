/*
 * Copyright 2017 WebAssembly Community Group participants
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

#ifndef wasm_ast_effects_h
#define wasm_ast_effects_h

namespace wasm {

// Look for side effects, including control flow
// TODO: optimize

struct EffectAnalyzer : public PostWalker<EffectAnalyzer> {
  EffectAnalyzer(PassOptions& passOptions, Expression *ast = nullptr) {
    ignoreImplicitTraps = passOptions.ignoreImplicitTraps;
    debugInfo = passOptions.debugInfo;
    if (ast) analyze(ast);
  }

  bool ignoreImplicitTraps;
  bool debugInfo;

  void analyze(Expression *ast) {
    breakNames.clear();
    walk(ast);
    // if we are left with breaks, they are external
    if (breakNames.size() > 0) branches = true;
  }

  bool branches = false; // branches out of this expression
  bool calls = false;
  std::set<Index> localsRead;
  std::set<Index> localsWritten;
  std::set<Name> globalsRead;
  std::set<Name> globalsWritten;
  bool readsMemory = false;
  bool writesMemory = false;
  bool implicitTrap = false; // a load or div/rem, which may trap. we ignore trap
                             // differences, so it is ok to reorder these, and we
                             // also allow reordering them with other effects
                             // (so a trap may occur later or earlier, if it is
                             // going to occur anyhow), but we can't remove them,
                             // they count as side effects

  bool accessesLocal() { return localsRead.size() + localsWritten.size() > 0; }
  bool accessesGlobal() { return globalsRead.size() + globalsWritten.size() > 0; }
  bool accessesMemory() { return calls || readsMemory || writesMemory; }
  bool hasSideEffects() { return calls || localsWritten.size() > 0 || writesMemory || branches || globalsWritten.size() > 0 || implicitTrap; }
  bool hasAnything() { return branches || calls || accessesLocal() || readsMemory || writesMemory || accessesGlobal() || implicitTrap; }

  // checks if these effects would invalidate another set (e.g., if we write, we invalidate someone that reads, they can't be moved past us)
  bool invalidates(EffectAnalyzer& other) {
    if (branches || other.branches
                 || ((writesMemory || calls) && other.accessesMemory())
                 || (accessesMemory() && (other.writesMemory || other.calls))) {
      return true;
    }
    for (auto local : localsWritten) {
      if (other.localsWritten.count(local) || other.localsRead.count(local)) {
        return true;
      }
    }
    for (auto local : localsRead) {
      if (other.localsWritten.count(local)) return true;
    }
    if ((accessesGlobal() && other.calls) || (other.accessesGlobal() && calls)) {
      return true;
    }
    for (auto global : globalsWritten) {
      if (other.globalsWritten.count(global) || other.globalsRead.count(global)) {
        return true;
      }
    }
    for (auto global : globalsRead) {
      if (other.globalsWritten.count(global)) return true;
    }
    // we are ok to reorder implicit traps, but not conditionalize them
    if ((implicitTrap && other.branches) || (other.implicitTrap && branches)) {
      return true;
    }
    return false;
  }

  void mergeIn(EffectAnalyzer& other) {
    branches = branches || other.branches;
    calls = calls || other.calls;
    readsMemory = readsMemory || other.readsMemory;
    writesMemory = writesMemory || other.writesMemory;
    for (auto i : other.localsRead) localsRead.insert(i);
    for (auto i : other.localsWritten) localsWritten.insert(i);
    for (auto i : other.globalsRead) globalsRead.insert(i);
    for (auto i : other.globalsWritten) globalsWritten.insert(i);
  }

  // the checks above happen after the node's children were processed, in the order of execution
  // we must also check for control flow that happens before the children, i.e., loops
  bool checkPre(Expression* curr) {
    if (curr->is<Loop>()) {
      branches = true;
      return true;
    }
    return false;
  }

  bool checkPost(Expression* curr) {
    visit(curr);
    if (curr->is<Loop>()) {
      branches = true;
    }
    return hasAnything();
  }

  std::set<Name> breakNames;

  void visitBreak(Break *curr) {
    breakNames.insert(curr->name);
  }
  void visitSwitch(Switch *curr) {
    for (auto name : curr->targets) {
      breakNames.insert(name);
    }
    breakNames.insert(curr->default_);
  }
  void visitBlock(Block* curr) {
    if (curr->name.is()) breakNames.erase(curr->name); // these were internal breaks
  }
  void visitLoop(Loop* curr) {
    if (curr->name.is()) breakNames.erase(curr->name); // these were internal breaks
  }

  void visitCall(Call *curr) { calls = true; }
  void visitCallImport(CallImport *curr) {
    calls = true;
    if (debugInfo) {
      // debugInfo call imports must be preserved very strongly, do not
      // move code around them
      branches = true; // !
    }
  }
  void visitCallIndirect(CallIndirect *curr) { calls = true; }
  void visitGetLocal(GetLocal *curr) {
    localsRead.insert(curr->index);
  }
  void visitSetLocal(SetLocal *curr) {
    localsWritten.insert(curr->index);
  }
  void visitGetGlobal(GetGlobal *curr) {
    globalsRead.insert(curr->name);
  }
  void visitSetGlobal(SetGlobal *curr) {
    globalsWritten.insert(curr->name);
  }
  void visitLoad(Load *curr) {
    readsMemory = true;
    if (!ignoreImplicitTraps) implicitTrap = true;
  }
  void visitStore(Store *curr) {
    writesMemory = true;
    if (!ignoreImplicitTraps) implicitTrap = true;
  }
  void visitUnary(Unary *curr) {
    if (!ignoreImplicitTraps) {
      switch (curr->op) {
        case TruncSFloat32ToInt32:
        case TruncSFloat32ToInt64:
        case TruncUFloat32ToInt32:
        case TruncUFloat32ToInt64:
        case TruncSFloat64ToInt32:
        case TruncSFloat64ToInt64:
        case TruncUFloat64ToInt32:
        case TruncUFloat64ToInt64: {
          implicitTrap = true;
        }
        default: {}
      }
    }
  }
  void visitBinary(Binary *curr) {
    if (!ignoreImplicitTraps) {
      switch (curr->op) {
        case DivSInt32:
        case DivUInt32:
        case RemSInt32:
        case RemUInt32:
        case DivSInt64:
        case DivUInt64:
        case RemSInt64:
        case RemUInt64: {
          implicitTrap = true;
        }
        default: {}
      }
    }
  }
  void visitReturn(Return *curr) { branches = true; }
  void visitHost(Host *curr) { calls = true; }
  void visitUnreachable(Unreachable *curr) { branches = true; }
};

} // namespace wasm

#endif // wasm_ast_effects_h

