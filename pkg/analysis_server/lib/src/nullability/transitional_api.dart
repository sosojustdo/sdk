// Copyright (c) 2019, the Dart project authors. Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

import 'package:analysis_server/src/nullability/conditional_discard.dart';
import 'package:analysis_server/src/nullability/constraint_gatherer.dart';
import 'package:analysis_server/src/nullability/constraint_variable_gatherer.dart';
import 'package:analysis_server/src/nullability/decorated_type.dart';
import 'package:analysis_server/src/nullability/expression_checks.dart';
import 'package:analysis_server/src/nullability/unit_propagation.dart';
import 'package:analyzer/dart/ast/ast.dart';
import 'package:analyzer/dart/element/element.dart';
import 'package:analyzer/src/generated/resolver.dart';
import 'package:analyzer/src/generated/source.dart';
import 'package:analyzer_plugin/protocol/protocol_common.dart' show SourceEdit;

/// Type of a [ConstraintVariable] representing the addition of a null check.
class CheckExpression extends ConstraintVariable {
  final int offset;

  CheckExpression(Expression expression) : offset = expression.end;

  @override
  toString() => 'checkNotNull($offset)';
}

/// Records information about how a conditional expression or statement might
/// need to be modified.
class ConditionalModification extends PotentialModification {
  final int offset;

  final int end;

  final bool isStatement;

  final ConditionalDiscard discard;

  final _KeepNode condition;

  final _KeepNode thenStatement;

  final _KeepNode elseStatement;

  @override
  final Source source;

  factory ConditionalModification(
      Source source, AstNode node, ConditionalDiscard discard) {
    if (node is IfStatement) {
      return ConditionalModification._(
          node.offset,
          node.end,
          node is Statement,
          discard,
          _KeepNode(node.condition),
          _KeepNode(node.thenStatement),
          _KeepNode(node.elseStatement),
          source);
    } else {
      throw new UnimplementedError('TODO(paulberry)');
    }
  }

  ConditionalModification._(
      this.offset,
      this.end,
      this.isStatement,
      this.discard,
      this.condition,
      this.thenStatement,
      this.elseStatement,
      this.source);

  @override
  bool get isEmpty => discard.keepTrue.value && discard.keepFalse.value;

  @override
  Iterable<SourceEdit> get modifications {
    if (isEmpty) return const [];
    // TODO(paulberry): move the following logic into DartEditBuilder (see
    // dartbug.com/35872).
    var result = <SourceEdit>[];
    var keepNodes = <_KeepNode>[];
    if (!discard.pureCondition) {
      keepNodes.add(condition); // TODO(paulberry): test
    }
    if (discard.keepTrue.value) {
      keepNodes.add(thenStatement); // TODO(paulberry): test
    }
    if (discard.keepFalse.value) {
      keepNodes.add(elseStatement); // TODO(paulberry): test
    }
    // TODO(paulberry): test thoroughly
    for (int i = 0; i < keepNodes.length; i++) {
      var keepNode = keepNodes[i];
      if (i == 0 && keepNode.offset != offset) {
        result.add(SourceEdit(offset, 0, '/* '));
      }
      if (i != 0 || keepNode.offset != offset) {
        result.add(SourceEdit(keepNode.offset, 0, '*/ '));
      }
      if (i != keepNodes.length - 1 || keepNode.end != end) {
        result.add(SourceEdit(keepNode.end, 0,
            keepNode.isExpression && isStatement ? '; /*' : ' /*'));
      }
      if (i == keepNodes.length - 1 && keepNode.end != end) {
        result.add(SourceEdit(end, 0, ' */'));
      }
    }
    return result;
  }
}

/// Enum encapsulating the various options proposed at
/// https://github.com/dart-lang/language/issues/156#issuecomment-460525075
enum DefaultParameterHandling {
  /// Option 2: Add required named parameters
  ///
  /// - `{int x}` implicitly means `x` is required
  ///   - required-ness goes into the function type:
  ///     `int Function({required int x})`
  /// - `{required int? x}` is allowed
  ///   - means that something must be passed
  ///   - passing null is allowed
  /// - `{int x = 3}` is allowed
  ///   - `x` is optional
  ///   - passing null to it is an error
  ///   - passing nothing to it results in it getting the default value
  /// - `[int x]` is an error
  /// - `[int x = 3]` is allowed
  option2_addRequiredNamedParameters,
}

/// Enum representing the possible heuristics for handling named parameters with
/// no default value.
enum NamedNoDefaultParameterHeuristic {
  /// Assume that the parameter should be considered nullable, unless the user
  /// has explicitly marked it as `@required`.
  assumeNullable,

  /// Assume that the parameter should be considered required, unless the user
  /// has explicitly marked it as nullable.
  assumeRequired,
}

/// Transitional migration API.
///
/// Usage: pass each input source file to [prepareInput].  Then pass each input
/// source file to [processInput].  Then call [finish] to obtain the
/// modifications that need to be made to each source file.
///
/// TODO(paulberry): this implementation keeps a lot of CompilationUnit objects
/// around.  Can we do better?
class NullabilityMigration {
  final bool _permissive;

  final NullabilityMigrationAssumptions assumptions;

  final _variables = Variables();

  final _constraints = Solver();

  /// Prepares to perform nullability migration.
  ///
  /// If [permissive] is `true`, exception handling logic will try to proceed
  /// as far as possible even though the migration algorithm is not yet
  /// complete.  TODO(paulberry): remove this mode once the migration algorithm
  /// is fully implemented.
  NullabilityMigration(
      {bool permissive: false,
      this.assumptions: const NullabilityMigrationAssumptions()})
      : _permissive = permissive;

  List<PotentialModification> finish() {
    _constraints.applyHeuristics();
    return _variables.getPotentialModifications();
  }

  void prepareInput(CompilationUnit unit) {
    unit.accept(ConstraintVariableGatherer(
        _variables, unit.declaredElement.source, _permissive, assumptions));
  }

  void processInput(CompilationUnit unit, TypeProvider typeProvider) {
    unit.accept(ConstraintGatherer(typeProvider, _variables, _constraints,
        unit.declaredElement.source, _permissive, assumptions));
  }
}

/// Assumptions affecting the behavior of the nullability migration tool.
///
/// These options generally reflect design decisions that have not yet been
/// made.  They don't reflect behavioral differences we would want to expose to
/// the user.
///
/// TODO(paulberry): hardcode these assumptions once decisions have been made.
class NullabilityMigrationAssumptions {
  /// Handling of default parameters.
  final DefaultParameterHandling defaultParameterHandling;

  /// Heuristic for handling named parameters with no default value.
  final NamedNoDefaultParameterHeuristic namedNoDefaultParameterHeuristic;

  const NullabilityMigrationAssumptions(
      {this.defaultParameterHandling:
          DefaultParameterHandling.option2_addRequiredNamedParameters,
      this.namedNoDefaultParameterHeuristic:
          NamedNoDefaultParameterHeuristic.assumeNullable});
}

/// Records information about the possible addition of a `@required` annotation
/// to the source code.
class PotentiallyAddRequired extends PotentialModification {
  @override
  final Source source;

  final ConstraintVariable _optionalVariable;

  final int _offset;

  PotentiallyAddRequired(
      this.source, DefaultFormalParameter parameter, this._optionalVariable)
      : _offset = parameter.offset;

  @override
  bool get isEmpty => _optionalVariable.value;

  @override
  Iterable<SourceEdit> get modifications =>
      isEmpty ? const [] : [SourceEdit(_offset, 0, '@required ')];
}

/// Interface used by data structures representing potential modifications to
/// the code being migrated.
abstract class PotentialModification {
  bool get isEmpty;

  /// Gets the individual migrations that need to be done, considering the
  /// solution to the constraint equations.
  Iterable<SourceEdit> get modifications;

  Source get source;
}

class Variables implements VariableRecorder, VariableRepository {
  final _decoratedElementTypes = <Element, DecoratedType>{};

  final _potentialModifications = <PotentialModification>[];

  @override
  DecoratedType decoratedElementType(Element element, {bool create: false}) =>
      _decoratedElementTypes[element] ??= create
          ? DecoratedType.forElement(element)
          : throw StateError('No element found');

  List<PotentialModification> getPotentialModifications() =>
      _potentialModifications.where((m) => !m.isEmpty).toList();

  @override
  void recordConditionalDiscard(
      Source source, AstNode node, ConditionalDiscard conditionalDiscard) {
    _potentialModifications
        .add(ConditionalModification(source, node, conditionalDiscard));
  }

  void recordDecoratedElementType(Element element, DecoratedType type) {
    _decoratedElementTypes[element] = type;
  }

  void recordDecoratedExpressionType(Expression node, DecoratedType type) {}

  void recordDecoratedTypeAnnotation(
      TypeAnnotation node, DecoratedTypeAnnotation type) {
    _potentialModifications.add(type);
  }

  @override
  void recordExpressionChecks(Expression expression, ExpressionChecks checks) {
    _potentialModifications.add(checks);
  }

  @override
  void recordPossiblyOptional(Source source, DefaultFormalParameter parameter,
      ConstraintVariable variable) {
    _potentialModifications
        .add(PotentiallyAddRequired(source, parameter, variable));
  }
}

/// Helper object used by [ConditionalModification] to keep track of AST nodes
/// within the conditional expression.
class _KeepNode {
  final int offset;

  final int end;

  final bool isExpression;

  factory _KeepNode(AstNode node) {
    int offset = node.offset;
    int end = node.end;
    if (node is Block && node.statements.isNotEmpty) {
      offset = node.statements.beginToken.offset;
      end = node.statements.endToken.end;
    }
    return _KeepNode._(offset, end, node is Expression);
  }

  _KeepNode._(this.offset, this.end, this.isExpression);
}
