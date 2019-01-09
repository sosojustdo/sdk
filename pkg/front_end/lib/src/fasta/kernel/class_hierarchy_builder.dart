// Copyright (c) 2018, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

library fasta.class_hierarchy_builder;

import 'package:kernel/ast.dart'
    show Library, Member, Name, Procedure, ProcedureKind;

import 'package:kernel/class_hierarchy.dart' show ClassHierarchy;

import '../messages.dart'
    show
        LocatedMessage,
        messageDeclaredMemberConflictsWithInheritedMember,
        messageDeclaredMemberConflictsWithInheritedMemberCause,
        messageInheritedMembersConflict,
        messageInheritedMembersConflictCause1,
        messageInheritedMembersConflictCause2,
        templateMissingImplementationCause,
        templateMissingImplementationNotAbstract;

import '../names.dart' show noSuchMethodName;

import '../scope.dart' show Scope;

import 'kernel_builder.dart'
    show
        Declaration,
        LibraryBuilder,
        KernelClassBuilder,
        KernelNamedTypeBuilder,
        KernelTypeBuilder;

int compareDeclarations(Declaration a, Declaration b) {
  return ClassHierarchy.compareMembers(a.target, b.target);
}

ProcedureKind memberKind(Member member) {
  return member is Procedure ? member.kind : null;
}

bool isNameVisibleIn(
    Name name, LibraryBuilder<KernelTypeBuilder, Library> library) {
  return !name.isPrivate || name.library == library.target;
}

class ClassHierarchyBuilder {
  final Map<KernelClassBuilder, ClassHierarchyNode> nodes =
      <KernelClassBuilder, ClassHierarchyNode>{};

  final KernelClassBuilder objectClass;

  bool hasNoSuchMethod = false;

  int abstractMemberCount = 0;

  ClassHierarchyBuilder(this.objectClass);

  /// A merge conflict arises when merging two lists that each have an element
  /// with the same name.
  ///
  /// If [mergeKind] is `MergeKind.superclass`, [a] should override [b].
  ///
  /// If [mergeKind] is `MergeKind.interfaces`, we need to record them and
  /// solve the conflict later.
  ///
  /// If [mergeKind] is `MergeKind.supertypes`, [a] should implement [b], and
  /// [b] is implicitly abstract.
  Declaration handleMergeConflict(KernelClassBuilder cls, Declaration a,
      Declaration b, MergeKind mergeKind) {
    if (a == b) return a;
    if (a.next != null || b.next != null) {
      // Don't check overrides involving duplicated members.
      return a;
    }
    Member aTarget = a.target;
    Member bTarget = b.target;
    if ((memberKind(aTarget) ?? ProcedureKind.Getter) !=
        (memberKind(bTarget) ?? ProcedureKind.Getter)) {
      String name = a.fullNameForErrors;
      if (mergeKind == MergeKind.interfaces) {
        cls.addProblem(messageInheritedMembersConflict, cls.charOffset,
            cls.fullNameForErrors.length,
            context: <LocatedMessage>[
              messageInheritedMembersConflictCause1.withLocation(
                  a.fileUri, a.charOffset, name.length),
              messageInheritedMembersConflictCause2.withLocation(
                  b.fileUri, b.charOffset, name.length),
            ]);
      } else {
        cls.addProblem(messageDeclaredMemberConflictsWithInheritedMember,
            a.charOffset, name.length,
            context: <LocatedMessage>[
              messageDeclaredMemberConflictsWithInheritedMemberCause
                  .withLocation(b.fileUri, b.charOffset, name.length)
            ]);
      }
    }
    if (mergeKind == MergeKind.superclass &&
        aTarget.name == noSuchMethodName &&
        !aTarget.isAbstract) {
      hasNoSuchMethod = true;
    }
    Declaration result = a;
    if (mergeKind == MergeKind.interfaces) {
      // TODO(ahe): Combine the signatures of a and b.
    } else if (aTarget.isAbstract) {
      if (mergeKind == MergeKind.superclass && !bTarget.isAbstract) {
        // An abstract method doesn't override an implemention inherited from a
        // superclass.
        result = b;
      } else {
        abstractMemberCount++;
      }
    }
    return result;
  }

  /// If [mergeKind] is `MergeKind.superclass` [member] is declared in current
  /// class, and isn't overriding a method from the superclass.
  ///
  /// If [mergeKind] is `MergeKind.interfaces`, [member] is ignored for now.
  ///
  /// If [mergeKind] is `MergeKind.supertypes`, [member] isn't
  /// implementing/overriding anything.
  void handleOnlyA(Declaration member, MergeKind mergeKind) {
    Member target = member.target;
    if (mergeKind == MergeKind.supertypes ||
        (mergeKind == MergeKind.superclass && target.isAbstract)) {
      abstractMemberCount++;
    }
  }

  /// If [mergeKind] is `MergeKind.superclass` [member] is being inherited from
  /// a superclass.
  ///
  /// If [mergeKind] is `MergeKind.interfaces`, [member] is ignored for now.
  ///
  /// If [mergeKind] is `MergeKind.supertypes`, [member] is implicitly
  /// abstract, and not implemented.
  void handleOnlyB(
      KernelClassBuilder cls, Declaration member, MergeKind mergeKind) {
    Member target = member.target;
    if (mergeKind == MergeKind.supertypes ||
        (mergeKind == MergeKind.superclass && target.isAbstract)) {
      if (isNameVisibleIn(target.name, cls.library)) {
        abstractMemberCount++;
      }
    }
    if (member.parent != objectClass &&
        target.name == noSuchMethodName &&
        !target.isAbstract) {
      hasNoSuchMethod = true;
    }
  }

  void add(KernelClassBuilder cls) {
    if (cls.isPatch) {
      // TODO(ahe): What about patch classes. Have we injected patched members
      // into the class-builder's scope?
      return;
    }
    ClassHierarchyNode supernode;
    if (objectClass != cls) {
      supernode = getNode(cls.supertype);
      if (supernode == null) {
        supernode = nodes[objectClass];
        if (supernode == null) {
          add(objectClass);
          supernode = nodes[objectClass];
        }
      }
      assert(supernode != null);
    }

    Scope scope = cls.scope;
    if (cls.isMixinApplication) {
      Declaration mixin = getDeclaration(cls.mixedInType);
      if (mixin is KernelClassBuilder) {
        scope = mixin.scope;
      }
    }
    List<Declaration> localMembers =
        new List<Declaration>.from(scope.local.values)
          ..sort(compareDeclarations);
    List<Declaration> localSetters =
        new List<Declaration>.from(scope.setters.values)
          ..sort(compareDeclarations);
    List<Declaration> classMembers;
    List<Declaration> classSetters;
    List<Declaration> interfaceMembers;
    List<Declaration> interfaceSetters;
    if (supernode == null) {
      // This should be Object.
      classMembers = localMembers;
      classSetters = localSetters;
    } else {
      classMembers = merge(
          cls, localMembers, supernode.classMembers, MergeKind.superclass);
      classSetters = merge(
          cls, localSetters, supernode.classSetters, MergeKind.superclass);
      List<KernelTypeBuilder> interfaces = cls.interfaces;
      if (interfaces != null) {
        MergeResult result = mergeInterfaces(cls, supernode, interfaces);
        interfaceMembers = result.mergedMembers;
        interfaceSetters = result.mergedSetters;
      } else {
        interfaceMembers = supernode.interfaceMembers;
        interfaceSetters = supernode.interfaceSetters;
      }
      if (interfaceMembers != null) {
        interfaceMembers =
            merge(cls, classMembers, interfaceMembers, MergeKind.supertypes);
      }
      if (interfaceMembers != null) {
        interfaceSetters =
            merge(cls, classSetters, interfaceSetters, MergeKind.supertypes);
      }
    }
    nodes[cls] = new ClassHierarchyNode(cls, scope, classMembers, classSetters,
        interfaceMembers, interfaceSetters);
    mergeAccessors(cls, classMembers, classSetters);

    if (abstractMemberCount != 0 && !cls.isAbstract) {
      if (!hasNoSuchMethod) {
        reportMissingMembers(cls, classMembers, classSetters);
      }
      installNsmHandlers(cls);
    }
    hasNoSuchMethod = false;
    abstractMemberCount = 0;
  }

  MergeResult mergeInterfaces(KernelClassBuilder cls,
      ClassHierarchyNode supernode, List<KernelTypeBuilder> interfaces) {
    List<List<Declaration>> memberLists =
        new List<List<Declaration>>(interfaces.length + 1);
    List<List<Declaration>> setterLists =
        new List<List<Declaration>>(interfaces.length + 1);
    memberLists[0] = supernode.interfaceMembers;
    setterLists[0] = supernode.interfaceSetters;
    for (int i = 0; i < interfaces.length; i++) {
      ClassHierarchyNode interfaceNode = getNode(interfaces[i]);
      if (interfaceNode == null) {
        memberLists[i + 1] = null;
        setterLists[i + 1] = null;
      } else {
        memberLists[i + 1] =
            interfaceNode.interfaceMembers ?? interfaceNode.classMembers;
        setterLists[i + 1] =
            interfaceNode.interfaceSetters ?? interfaceNode.classSetters;
      }
    }
    return new MergeResult(
        mergeLists(cls, memberLists), mergeLists(cls, setterLists));
  }

  List<Declaration> mergeLists(
      KernelClassBuilder cls, List<List<Declaration>> input) {
    // This is a k-way merge sort (where k is `input.length + 1`). We merge the
    // lists pairwise, which reduces the number of lists to merge by half on
    // each iteration. Consequently, we perform O(log k) merges.
    while (input.length > 1) {
      List<List<Declaration>> output = <List<Declaration>>[];
      for (int i = 0; i < input.length - 1; i += 2) {
        List<Declaration> first = input[i];
        List<Declaration> second = input[i + 1];
        if (first == null) {
          output.add(second);
        } else if (second == null) {
          output.add(first);
        } else {
          output.add(merge(cls, first, second, MergeKind.interfaces));
        }
      }
      if (input.length.isOdd) {
        output.add(input.last);
      }
      input = output;
    }
    return input.single;
  }

  /// Merge [and check] accessors. This entails removing setters corresponding
  /// to fields, and checking that setters don't override regular methods.
  void mergeAccessors(KernelClassBuilder cls, List<Declaration> members,
      List<Declaration> setters) {
    List<Declaration> overriddenSetters;
    int i = 0;
    int j = 0;
    while (i < members.length && j < setters.length) {
      Declaration member = members[i];
      Declaration setter = setters[j];
      final int compare = compareDeclarations(member, setter);
      if (compare == 0) {
        if (member.isField) {
          // TODO(ahe): What happens if we have both a field and a setter
          // declared in the same class?
          if (!member.isFinal && !member.isConst) {
            // The field overrides the setter.
            (overriddenSetters ??= <Declaration>[]).add(setter);
            Member target = setter.target;
            if (target.isAbstract) {
              abstractMemberCount--;
            }
          }
        } else if (!member.isGetter) {
          String name = member.fullNameForErrors;
          cls.library.addProblem(
              messageDeclaredMemberConflictsWithInheritedMember,
              member.charOffset,
              name.length,
              member.fileUri,
              context: <LocatedMessage>[
                messageDeclaredMemberConflictsWithInheritedMemberCause
                    .withLocation(
                        setter.fileUri, setter.charOffset, name.length)
              ]);
        }
        i++;
        j++;
      } else if (compare < 0) {
        i++;
      } else {
        j++;
      }
    }
    // One of of the two lists is now exhausted. What remains in the other list
    // cannot be a conflict.

    if (overriddenSetters != null) {
      // Remove [overriddenSetters] from [setters] by copying [setters]
      // to itself.
      int i = 0;
      int j = 0;
      int storeIndex = 0;
      while (i < setters.length && j < overriddenSetters.length) {
        if (setters[i] == overriddenSetters[j]) {
          i++;
          j++;
        } else {
          setters[storeIndex++] = setters[i++];
        }
      }
      while (i < setters.length) {
        setters[storeIndex++] = setters[i++];
      }
      setters.length = storeIndex;
    }
  }

  void reportMissingMembers(KernelClassBuilder cls, List<Declaration> members,
      List<Declaration> setters) {
    List<LocatedMessage> context = <LocatedMessage>[];
    List<String> missingNames = <String>[];
    for (int j = 0; j < 2; j++) {
      List<Declaration> declarations = j == 0 ? members : setters;
      for (int i = 0; i < declarations.length; i++) {
        Declaration declaration = declarations[i];
        Member target = declaration.target;
        if (target.isAbstract && isNameVisibleIn(target.name, cls.library)) {
          String name = declaration.fullNameForErrors;
          String parentName = declaration.parent.fullNameForErrors;
          String displayName =
              declaration.isSetter ? "$parentName.$name=" : "$parentName.$name";
          missingNames.add(displayName);
          context.add(templateMissingImplementationCause
              .withArguments(displayName)
              .withLocation(
                  declaration.fileUri, declaration.charOffset, name.length));
        }
      }
    }
    if (missingNames.isEmpty) return;
    cls.addProblem(
        templateMissingImplementationNotAbstract.withArguments(
            cls.fullNameForErrors, missingNames),
        cls.charOffset,
        cls.fullNameForErrors.length,
        context: context);
  }

  void installNsmHandlers(KernelClassBuilder cls) {
    // TOOD(ahe): Implement this.
  }

  ClassHierarchyNode getNode(KernelTypeBuilder type) {
    Declaration declaration = getDeclaration(type);
    if (declaration is KernelClassBuilder) {
      ClassHierarchyNode node = nodes[declaration];
      if (node == null) {
        add(declaration);
        node = nodes[declaration];
      }
      return node;
    }
    return null;
  }

  Declaration getDeclaration(KernelTypeBuilder type) {
    return type is KernelNamedTypeBuilder ? type.declaration : null;
  }

  List<Declaration> merge(KernelClassBuilder cls, List<Declaration> aList,
      List<Declaration> bList, MergeKind mergeKind) {
    final List<Declaration> result = new List<Declaration>.filled(
        aList.length + bList.length, null,
        growable: true);

    int storeIndex = 0;

    int i = 0;
    int j = 0;
    while (i < aList.length && j < bList.length) {
      final Declaration a = aList[i];
      final Declaration b = bList[j];
      final int compare = compareDeclarations(a, b);
      if (compare == 0) {
        result[storeIndex++] = handleMergeConflict(cls, a, b, mergeKind);
        i++;
        j++;
      } else if (compare < 0) {
        handleOnlyA(a, mergeKind);
        result[storeIndex++] = a;
        i++;
      } else {
        handleOnlyB(cls, b, mergeKind);
        result[storeIndex++] = b;
        j++;
      }
    }
    while (i < aList.length) {
      final Declaration a = aList[i];
      handleOnlyA(a, mergeKind);
      result[storeIndex++] = a;
      i++;
    }
    while (j < bList.length) {
      final Declaration b = bList[j];
      handleOnlyB(cls, b, mergeKind);
      result[storeIndex++] = b;
      j++;
    }
    return result..length = storeIndex;
  }
}

class ClassHierarchyNode {
  /// The class corresponding to this hierarchy node.
  final KernelClassBuilder cls;

  /// The local members of [cls]. For regular classes, this is simply
  /// `cls.scope`, but for mixin-applications this is the mixed-in type's
  /// scope. The members are sorted in order of declaration.
  // TODO(ahe): Do we need to copy the scope from the mixed-in type to remove
  // static members?
  final Scope localMembers;

  /// All the members of this class including [classMembers] of its
  /// superclasses. The members are sorted by [compareDeclarations].
  final List<Declaration> classMembers;

  /// Similar to [classMembers] but for setters.
  final List<Declaration> classSetters;

  /// All the interface members of this class including [interfaceMembers] of
  /// its supertypes. The members are sorted by [compareDeclarations].
  ///
  /// In addition to the members of [classMembers] this also contains members
  /// from interfaces.
  final List<Declaration> interfaceMembers;

  /// Similar to [interfaceMembers] but for setters.
  final List<Declaration> interfaceSetters;

  ClassHierarchyNode(this.cls, this.localMembers, this.classMembers,
      this.classSetters, this.interfaceMembers, this.interfaceSetters);
}

class MergeResult {
  final List<Declaration> mergedMembers;

  final List<Declaration> mergedSetters;

  MergeResult(this.mergedMembers, this.mergedSetters);
}

enum MergeKind {
  /// Merging superclass members with the current class.
  superclass,

  /// Merging two interfaces.
  interfaces,

  /// Merging class members with interface members.
  supertypes,
}
