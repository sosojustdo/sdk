// Copyright (c) 2016, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

library fasta.kernel_library_builder;

import 'package:kernel/ast.dart';

import 'package:kernel/clone.dart' show CloneVisitor;

import '../errors.dart' show internalError;

import '../loader.dart' show Loader;

import '../modifier.dart' show abstractMask, staticMask;

import '../source/source_library_builder.dart'
    show DeclarationBuilder, SourceLibraryBuilder;

import '../source/source_class_builder.dart' show SourceClassBuilder;

import '../util/relativize.dart' show relativizeUri;

import 'kernel_builder.dart'
    show
        Builder,
        ClassBuilder,
        ConstructorReferenceBuilder,
        DynamicTypeBuilder,
        FormalParameterBuilder,
        FunctionTypeAliasBuilder,
        KernelConstructorBuilder,
        KernelEnumBuilder,
        KernelFieldBuilder,
        KernelFormalParameterBuilder,
        KernelFunctionTypeAliasBuilder,
        KernelFunctionTypeBuilder,
        KernelInvalidTypeBuilder,
        KernelMixinApplicationBuilder,
        KernelNamedMixinApplicationBuilder,
        KernelNamedTypeBuilder,
        KernelProcedureBuilder,
        KernelTypeBuilder,
        KernelTypeVariableBuilder,
        MemberBuilder,
        MetadataBuilder,
        MixedAccessor,
        NamedMixinApplicationBuilder,
        PrefixBuilder,
        ProcedureBuilder,
        TypeBuilder,
        TypeVariableBuilder;

class KernelLibraryBuilder
    extends SourceLibraryBuilder<KernelTypeBuilder, Library> {
  final Library library;

  final Map<String, SourceClassBuilder> mixinApplicationClasses =
      <String, SourceClassBuilder>{};

  final List<List> argumentsWithMissingDefaultValues = <List>[];

  final List<KernelProcedureBuilder> nativeMethods = <KernelProcedureBuilder>[];

  final List<KernelTypeVariableBuilder> boundlessTypeVariables =
      <KernelTypeVariableBuilder>[];

  KernelLibraryBuilder(Uri uri, Uri fileUri, Loader loader)
      : library = new Library(uri, fileUri: relativizeUri(fileUri)),
        super(loader, fileUri);

  Library get target => library;

  Uri get uri => library.importUri;

  KernelTypeBuilder addNamedType(
      String name, List<KernelTypeBuilder> arguments, int charOffset) {
    KernelNamedTypeBuilder type =
        new KernelNamedTypeBuilder(name, arguments, charOffset, fileUri);
    if (identical(name, "dynamic")) {
      type.builder =
          new DynamicTypeBuilder(const DynamicType(), this, charOffset);
    } else {
      addType(type);
    }
    return type;
  }

  KernelTypeBuilder addMixinApplication(KernelTypeBuilder supertype,
      List<KernelTypeBuilder> mixins, int charOffset) {
    KernelTypeBuilder type = new KernelMixinApplicationBuilder(
        supertype, mixins, this, charOffset, fileUri);
    return addType(type);
  }

  KernelTypeBuilder addVoidType(int charOffset) {
    return new KernelNamedTypeBuilder("void", null, charOffset, fileUri);
  }

  void addClass(
      List<MetadataBuilder> metadata,
      int modifiers,
      String className,
      List<TypeVariableBuilder> typeVariables,
      KernelTypeBuilder supertype,
      List<KernelTypeBuilder> interfaces,
      int charOffset) {
    ClassBuilder cls = new SourceClassBuilder(
        metadata,
        modifiers,
        className,
        typeVariables,
        supertype,
        interfaces,
        classMembers,
        this,
        new List<ConstructorReferenceBuilder>.from(constructorReferences),
        charOffset);
    constructorReferences.clear();
    classMembers.forEach((String name, MemberBuilder builder) {
      while (builder != null) {
        builder.parent = cls;
        builder = builder.next;
      }
    });
    // Nested declaration began in `OutlineBuilder.beginClassDeclaration`.
    endNestedDeclaration().resolveTypes(typeVariables, this);
    addBuilder(className, cls, charOffset);
  }

  void addNamedMixinApplication(
      List<MetadataBuilder> metadata,
      String name,
      List<TypeVariableBuilder> typeVariables,
      int modifiers,
      KernelTypeBuilder mixinApplication,
      List<KernelTypeBuilder> interfaces,
      int charOffset) {
    NamedMixinApplicationBuilder builder =
        new KernelNamedMixinApplicationBuilder(metadata, name, typeVariables,
            modifiers, mixinApplication, interfaces, this, charOffset);
    // Nested declaration began in `OutlineBuilder.beginNamedMixinApplication`.
    endNestedDeclaration().resolveTypes(typeVariables, this);
    addBuilder(name, builder, charOffset);
  }

  void addField(List<MetadataBuilder> metadata, int modifiers,
      KernelTypeBuilder type, String name, int charOffset) {
    addBuilder(
        name,
        new KernelFieldBuilder(
            metadata, type, name, modifiers, this, charOffset),
        charOffset);
  }

  void addProcedure(
      List<MetadataBuilder> metadata,
      int modifiers,
      KernelTypeBuilder returnType,
      String name,
      List<TypeVariableBuilder> typeVariables,
      List<FormalParameterBuilder> formals,
      AsyncMarker asyncModifier,
      ProcedureKind kind,
      int charOffset,
      int charEndOffset,
      String nativeMethodName,
      {bool isTopLevel}) {
    // Nested declaration began in `OutlineBuilder.beginMethod` or
    // `OutlineBuilder.beginTopLevelMethod`.
    endNestedDeclaration().resolveTypes(typeVariables, this);
    ProcedureBuilder procedure;
    if (!isTopLevel && isConstructorName(name, currentDeclaration.name)) {
      int index = name.indexOf(".");
      name = index == -1 ? "" : name.substring(index + 1);
      procedure = new KernelConstructorBuilder(
          metadata,
          modifiers & ~abstractMask,
          returnType,
          name,
          typeVariables,
          formals,
          this,
          charOffset,
          charEndOffset,
          nativeMethodName);
    } else {
      procedure = new KernelProcedureBuilder(
          metadata,
          modifiers,
          returnType,
          name,
          typeVariables,
          formals,
          asyncModifier,
          kind,
          this,
          charOffset,
          charEndOffset,
          nativeMethodName);
    }
    addBuilder(name, procedure, charOffset);
    if (nativeMethodName != null) {
      addNativeMethod(procedure);
    }
  }

  void addFactoryMethod(
      List<MetadataBuilder> metadata,
      int modifiers,
      ConstructorReferenceBuilder constructorName,
      List<FormalParameterBuilder> formals,
      AsyncMarker asyncModifier,
      ConstructorReferenceBuilder redirectionTarget,
      int charOffset,
      int charEndOffset,
      String nativeMethodName) {
    // Nested declaration began in `OutlineBuilder.beginFactoryMethod`.
    DeclarationBuilder<KernelTypeBuilder> factoryDeclaration =
        endNestedDeclaration();
    String name = constructorName.name;
    int index = name.indexOf(".");
    name = index == -1 ? "" : name.substring(index + 1);
    assert(constructorName.suffix == null);
    KernelProcedureBuilder procedure = new KernelProcedureBuilder(
        metadata,
        staticMask | modifiers,
        null,
        name,
        <TypeVariableBuilder>[],
        formals,
        asyncModifier,
        ProcedureKind.Factory,
        this,
        charOffset,
        charEndOffset,
        nativeMethodName,
        redirectionTarget);
    currentDeclaration.addFactoryDeclaration(procedure, factoryDeclaration);
    addBuilder(name, procedure, charOffset);
    if (nativeMethodName != null) {
      addNativeMethod(procedure);
    }
  }

  void addEnum(List<MetadataBuilder> metadata, String name,
      List<String> constants, int charOffset, int charEndOffset) {
    addBuilder(
        name,
        new KernelEnumBuilder(
            metadata, name, constants, this, charOffset, charEndOffset),
        charOffset);
  }

  void addFunctionTypeAlias(
      List<MetadataBuilder> metadata,
      KernelTypeBuilder returnType,
      String name,
      List<TypeVariableBuilder> typeVariables,
      List<FormalParameterBuilder> formals,
      int charOffset) {
    FunctionTypeAliasBuilder typedef = new KernelFunctionTypeAliasBuilder(
        metadata, returnType, name, typeVariables, formals, this, charOffset);
    // Nested declaration began in `OutlineBuilder.beginFunctionTypeAlias`.
    endNestedDeclaration().resolveTypes(typeVariables, this);
    addBuilder(name, typedef, charOffset);
  }

  KernelFunctionTypeBuilder addFunctionType(
      KernelTypeBuilder returnType,
      List<TypeVariableBuilder> typeVariables,
      List<FormalParameterBuilder> formals,
      int charOffset) {
    return new KernelFunctionTypeBuilder(
        charOffset, fileUri, returnType, typeVariables, formals);
  }

  KernelFormalParameterBuilder addFormalParameter(
      List<MetadataBuilder> metadata,
      int modifiers,
      KernelTypeBuilder type,
      String name,
      bool hasThis,
      int charOffset) {
    return new KernelFormalParameterBuilder(
        metadata, modifiers, type, name, hasThis, this, charOffset);
  }

  KernelTypeVariableBuilder addTypeVariable(
      String name, KernelTypeBuilder bound, int charOffset) {
    var builder = new KernelTypeVariableBuilder(name, this, charOffset, bound);
    boundlessTypeVariables.add(builder);
    return builder;
  }

  void buildBuilder(Builder builder) {
    if (builder is SourceClassBuilder) {
      Class cls = builder.build(this);
      library.addClass(cls);
    } else if (builder is KernelFieldBuilder) {
      library.addMember(builder.build(this)..isStatic = true);
    } else if (builder is KernelProcedureBuilder) {
      library.addMember(builder.build(this)..isStatic = true);
    } else if (builder is FunctionTypeAliasBuilder) {
      // Kernel discard typedefs and use their corresponding function types
      // directly.
    } else if (builder is KernelEnumBuilder) {
      library.addClass(builder.build(this));
    } else if (builder is PrefixBuilder) {
      // Ignored. Kernel doesn't represent prefixes.
    } else {
      internalError("Unhandled builder: ${builder.runtimeType}");
    }
  }

  Library build() {
    super.build();
    library.name = name;
    return library;
  }

  Builder buildAmbiguousBuilder(
      String name, Builder builder, Builder other, int charOffset) {
    if (builder.next == null && other.next == null) {
      if (builder.isGetter && other.isSetter) {
        return new MixedAccessor(builder, other, this);
      } else if (builder.isSetter && other.isGetter) {
        return new MixedAccessor(other, builder, this);
      }
    }
    return new KernelInvalidTypeBuilder(name, charOffset, fileUri);
  }

  void addArgumentsWithMissingDefaultValues(
      Arguments arguments, FunctionNode function) {
    assert(partOfLibrary == null);
    argumentsWithMissingDefaultValues.add([arguments, function]);
  }

  int finishStaticInvocations() {
    CloneVisitor cloner;
    for (var list in argumentsWithMissingDefaultValues) {
      final Arguments arguments = list[0];
      final FunctionNode function = list[1];

      Expression defaultArgumentFrom(Expression expression) {
        if (expression == null) {
          return new NullLiteral();
        }
        cloner ??= new CloneVisitor();
        return cloner.clone(expression);
      }

      for (int i = function.requiredParameterCount;
          i < function.positionalParameters.length;
          i++) {
        arguments.positional[i] ??=
            defaultArgumentFrom(function.positionalParameters[i].initializer)
              ..parent = arguments;
      }
      Map<String, VariableDeclaration> names;
      for (NamedExpression expression in arguments.named) {
        if (expression.value == null) {
          if (names == null) {
            names = <String, VariableDeclaration>{};
            for (VariableDeclaration parameter in function.namedParameters) {
              names[parameter.name] = parameter;
            }
          }
          expression.value =
              defaultArgumentFrom(names[expression.name].initializer)
                ..parent = expression;
        }
      }
    }
    return argumentsWithMissingDefaultValues.length;
  }

  void addNativeMethod(KernelProcedureBuilder method) {
    nativeMethods.add(method);
  }

  int finishNativeMethods() {
    for (KernelProcedureBuilder method in nativeMethods) {
      method.becomeNative(loader);
    }
    return nativeMethods.length;
  }

  List<TypeVariableBuilder> copyTypeVariables(
      List<TypeVariableBuilder> original) {
    List<TypeVariableBuilder> copy = <TypeVariableBuilder>[];
    for (KernelTypeVariableBuilder variable in original) {
      var newVariable = new KernelTypeVariableBuilder(
          variable.name, this, variable.charOffset);
      copy.add(newVariable);
      boundlessTypeVariables.add(newVariable);
    }
    Map<TypeVariableBuilder, TypeBuilder> substitution =
        <TypeVariableBuilder, TypeBuilder>{};
    int i = 0;
    for (KernelTypeVariableBuilder variable in original) {
      substitution[variable] = copy[i++].asTypeBuilder();
    }
    i = 0;
    for (KernelTypeVariableBuilder variable in original) {
      copy[i++].bound = variable.bound?.subst(substitution);
    }
    return copy;
  }

  int finishTypeVariables(ClassBuilder object) {
    int count = boundlessTypeVariables.length;
    for (KernelTypeVariableBuilder builder in boundlessTypeVariables) {
      builder.finish(this, object);
    }
    boundlessTypeVariables.clear();
    return count;
  }

  @override
  void includePart(covariant KernelLibraryBuilder part) {
    super.includePart(part);
    nativeMethods.addAll(part.nativeMethods);
    boundlessTypeVariables.addAll(part.boundlessTypeVariables);
    assert(mixinApplicationClasses.isEmpty);
  }
}

bool isConstructorName(String name, String className) {
  if (name.startsWith(className)) {
    if (name.length == className.length) return true;
    if (name.startsWith(".", className.length)) return true;
  }
  return false;
}
