// mep's C# syntax facts: a translation of Roslyn's SyntaxFacts -- Syntax/SyntaxKindFacts.cs (in its order),
// then Parser/CharacterInfo.cs, Core's UnicodeCharacterUtilities.cs, and the green-token helpers of
// Syntax/SyntaxFacts.cs -- plus .NET's char.GetUnicodeCategory from a generated table.
//
// Roslyn is MIT-licensed: Copyright (c) .NET Foundation and Contributors
// (https://github.com/dotnet/roslyn/blob/main/License.txt).

#include "csharp_syntax.h"

#include <algorithm>
#include <initializer_list>
#include <unordered_map>
#include <utility>
#include <vector>

namespace csharp {

namespace {

/** @brief A C# `switch (text) { case "...": return SyntaxKind.X; ... default: return SyntaxKind.None; }`. */
class KeywordTable {
public:
    KeywordTable(std::initializer_list<std::pair<Text, SyntaxKind>> entries) : map_(entries.begin(), entries.end()) {}
    SyntaxKind Find(Text text) const {
        auto it = map_.find(text);
        return it == map_.end() ? SyntaxKind::None : it->second;
    }

private:
    std::unordered_map<Text, SyntaxKind> map_;  // keys are static literals
};

}  // namespace

namespace SyntaxFacts {

static bool IsDebuggerSpecialPunctuation(SyntaxKind kind);

// ---- Syntax/SyntaxKindFacts.cs ----------------------------------------------------------------------------

bool IsKeywordKind(SyntaxKind kind)
{
    return IsReservedKeyword(kind) || IsContextualKeyword(kind);
}

std::vector<SyntaxKind> GetReservedKeywordKinds()
{
    std::vector<SyntaxKind> result;
    for (int i = static_cast<int>(SyntaxKind::BoolKeyword); i <= static_cast<int>(SyntaxKind::ImplicitKeyword); i++)
    {
        result.push_back(static_cast<SyntaxKind>(i));
    }
    return result;
}

std::vector<SyntaxKind> GetKeywordKinds()
{
    std::vector<SyntaxKind> result;
    for (SyntaxKind reserved : GetReservedKeywordKinds())
    {
        result.push_back(reserved);
    }

    for (SyntaxKind contextual : GetContextualKeywordKinds())
    {
        result.push_back(contextual);
    }
    return result;
}

bool IsReservedKeyword(SyntaxKind kind)
{
    return kind >= SyntaxKind::BoolKeyword && kind <= SyntaxKind::ImplicitKeyword;
}

bool IsAttributeTargetSpecifier(SyntaxKind kind)
{
    switch (kind)
    {
        case SyntaxKind::AssemblyKeyword:
        case SyntaxKind::ModuleKeyword:
        case SyntaxKind::EventKeyword:
        case SyntaxKind::FieldKeyword:
        case SyntaxKind::MethodKeyword:
        case SyntaxKind::ParamKeyword:
        case SyntaxKind::PropertyKeyword:
        case SyntaxKind::ReturnKeyword:
        case SyntaxKind::TypeKeyword:
        case SyntaxKind::TypeVarKeyword:
            return true;
        default:
            return false;
    }
}

bool IsAccessibilityModifier(SyntaxKind kind)
{
    switch (kind)
    {
        case SyntaxKind::PrivateKeyword:
        case SyntaxKind::ProtectedKeyword:
        case SyntaxKind::InternalKeyword:
        case SyntaxKind::PublicKeyword:
            return true;
        default:
            return false;
    }
}

bool IsPreprocessorKeyword(SyntaxKind kind)
{
    switch (kind)
    {
        case SyntaxKind::TrueKeyword:
        case SyntaxKind::FalseKeyword:
        case SyntaxKind::DefaultKeyword:
        case SyntaxKind::IfKeyword:
        case SyntaxKind::ElseKeyword:
        case SyntaxKind::ElifKeyword:
        case SyntaxKind::EndIfKeyword:
        case SyntaxKind::RegionKeyword:
        case SyntaxKind::EndRegionKeyword:
        case SyntaxKind::DefineKeyword:
        case SyntaxKind::UndefKeyword:
        case SyntaxKind::WarningKeyword:
        case SyntaxKind::ErrorKeyword:
        case SyntaxKind::LineKeyword:
        case SyntaxKind::PragmaKeyword:
        case SyntaxKind::HiddenKeyword:
        case SyntaxKind::ChecksumKeyword:
        case SyntaxKind::DisableKeyword:
        case SyntaxKind::RestoreKeyword:
        case SyntaxKind::ReferenceKeyword:
        case SyntaxKind::LoadKeyword:
        case SyntaxKind::NullableKeyword:
        case SyntaxKind::EnableKeyword:
        case SyntaxKind::WarningsKeyword:
        case SyntaxKind::AnnotationsKeyword:
            return true;
        default:
            return false;
    }
}

/// <summary>
/// Some preprocessor keywords are only keywords when they appear after a
/// hash sign (#).  For these keywords, the lexer will produce tokens with
/// Kind = SyntaxKind::IdentifierToken and ContextualKind set to the keyword
/// SyntaxKind::
/// </summary>
/// <remarks>
/// This wrinkle is specifically not publicly exposed.
/// </remarks>
bool IsPreprocessorContextualKeyword(SyntaxKind kind)
{
    switch (kind)
    {
        case SyntaxKind::TrueKeyword:
        case SyntaxKind::FalseKeyword:
        case SyntaxKind::DefaultKeyword:
        case SyntaxKind::HiddenKeyword:
        case SyntaxKind::ChecksumKeyword:
        case SyntaxKind::DisableKeyword:
        case SyntaxKind::RestoreKeyword:
        case SyntaxKind::EnableKeyword:
        case SyntaxKind::WarningsKeyword:
        case SyntaxKind::AnnotationsKeyword:
            return false;
        default:
            return IsPreprocessorKeyword(kind);
    }
}

std::vector<SyntaxKind> GetPreprocessorKeywordKinds()
{
    std::vector<SyntaxKind> result;
    result.push_back(SyntaxKind::TrueKeyword);
    result.push_back(SyntaxKind::FalseKeyword);
    result.push_back(SyntaxKind::DefaultKeyword);

    for (int i = static_cast<int>(SyntaxKind::ElifKeyword); i <= static_cast<int>(SyntaxKind::RestoreKeyword); i++)
    {
        result.push_back(static_cast<SyntaxKind>(i));
    }
    return result;
}

bool IsPunctuation(SyntaxKind kind)
{
    return kind >= SyntaxKind::TildeToken && kind <= SyntaxKind::GreaterThanGreaterThanGreaterThanEqualsToken;
}

bool IsLanguagePunctuation(SyntaxKind kind)
{
    return IsPunctuation(kind) && !IsPreprocessorKeyword(kind) && !IsDebuggerSpecialPunctuation(kind);
}

bool IsPreprocessorPunctuation(SyntaxKind kind)
{
    return kind == SyntaxKind::HashToken;
}

static bool IsDebuggerSpecialPunctuation(SyntaxKind kind)
{
    // TODO: What about u"<>f_AnonymousMethod"? Or u"123#"? What's this used for?
    return kind == SyntaxKind::DollarToken;
}

std::vector<SyntaxKind> GetPunctuationKinds()
{
    std::vector<SyntaxKind> result;
    for (int i = static_cast<int>(SyntaxKind::TildeToken); i <= static_cast<int>(SyntaxKind::DotDotToken); i++)
    {
        result.push_back(static_cast<SyntaxKind>(i));
    }

    for (int i = static_cast<int>(SyntaxKind::SlashGreaterThanToken); i <= static_cast<int>(SyntaxKind::XmlProcessingInstructionEndToken); i++)
    {
        result.push_back(static_cast<SyntaxKind>(i));
    }

    for (int i = static_cast<int>(SyntaxKind::BarBarToken); i <= static_cast<int>(SyntaxKind::QuestionQuestionEqualsToken); i++)
    {
        result.push_back(static_cast<SyntaxKind>(i));
    }

    result.push_back(SyntaxKind::GreaterThanGreaterThanGreaterThanToken);
    result.push_back(SyntaxKind::GreaterThanGreaterThanGreaterThanEqualsToken);
    return result;
}

bool IsPunctuationOrKeyword(SyntaxKind kind)
{
    return kind >= SyntaxKind::TildeToken && kind <= SyntaxKind::EndOfFileToken;
}

bool IsLiteral(SyntaxKind kind)
{
    switch (kind)
    {
        case SyntaxKind::IdentifierToken:
        case SyntaxKind::StringLiteralToken:
        case SyntaxKind::Utf8StringLiteralToken:
        case SyntaxKind::SingleLineRawStringLiteralToken:
        case SyntaxKind::Utf8SingleLineRawStringLiteralToken:
        case SyntaxKind::MultiLineRawStringLiteralToken:
        case SyntaxKind::Utf8MultiLineRawStringLiteralToken:
        case SyntaxKind::CharacterLiteralToken:
        case SyntaxKind::NumericLiteralToken:
        case SyntaxKind::XmlTextLiteralToken:
        case SyntaxKind::XmlTextLiteralNewLineToken:
        case SyntaxKind::XmlEntityLiteralToken:
            return true;
        default:
            return false;
    }
}

bool IsAnyToken(SyntaxKind kind)
{
    if (kind >= SyntaxKind::TildeToken && kind < SyntaxKind::EndOfLineTrivia) return true;
    switch (kind)
    {
        case SyntaxKind::InterpolatedStringToken:
        case SyntaxKind::InterpolatedStringStartToken:
        case SyntaxKind::InterpolatedVerbatimStringStartToken:
        case SyntaxKind::InterpolatedMultiLineRawStringStartToken:
        case SyntaxKind::InterpolatedSingleLineRawStringStartToken:
        case SyntaxKind::InterpolatedStringTextToken:
        case SyntaxKind::InterpolatedStringEndToken:
        case SyntaxKind::InterpolatedRawStringEndToken:
        case SyntaxKind::LoadKeyword:
        case SyntaxKind::NullableKeyword:
        case SyntaxKind::EnableKeyword:
        case SyntaxKind::UnderscoreToken:
        case SyntaxKind::MultiLineRawStringLiteralToken:
        case SyntaxKind::SingleLineRawStringLiteralToken:
            return true;
        default:
            return false;
    }
}

bool IsTrivia(SyntaxKind kind)
{
    switch (kind)
    {
        case SyntaxKind::EndOfLineTrivia:
        case SyntaxKind::WhitespaceTrivia:
        case SyntaxKind::SingleLineCommentTrivia:
        case SyntaxKind::MultiLineCommentTrivia:
        case SyntaxKind::SingleLineDocumentationCommentTrivia:
        case SyntaxKind::MultiLineDocumentationCommentTrivia:
        case SyntaxKind::DisabledTextTrivia:
        case SyntaxKind::DocumentationCommentExteriorTrivia:
        case SyntaxKind::ConflictMarkerTrivia:
            return true;
        default:
            return IsPreprocessorDirective(kind);
    }
}

bool IsPreprocessorDirective(SyntaxKind kind)
{
    switch (kind)
    {
        case SyntaxKind::IfDirectiveTrivia:
        case SyntaxKind::ElifDirectiveTrivia:
        case SyntaxKind::ElseDirectiveTrivia:
        case SyntaxKind::EndIfDirectiveTrivia:
        case SyntaxKind::RegionDirectiveTrivia:
        case SyntaxKind::EndRegionDirectiveTrivia:
        case SyntaxKind::DefineDirectiveTrivia:
        case SyntaxKind::UndefDirectiveTrivia:
        case SyntaxKind::ErrorDirectiveTrivia:
        case SyntaxKind::WarningDirectiveTrivia:
        case SyntaxKind::LineDirectiveTrivia:
        case SyntaxKind::LineSpanDirectiveTrivia:
        case SyntaxKind::PragmaWarningDirectiveTrivia:
        case SyntaxKind::PragmaChecksumDirectiveTrivia:
        case SyntaxKind::ReferenceDirectiveTrivia:
        case SyntaxKind::LoadDirectiveTrivia:
        case SyntaxKind::BadDirectiveTrivia:
        case SyntaxKind::ShebangDirectiveTrivia:
        case SyntaxKind::IgnoredDirectiveTrivia:
        case SyntaxKind::NullableDirectiveTrivia:
            return true;
        default:
            return false;
    }
}

bool IsName(SyntaxKind kind)
{
    switch (kind)
    {
        case SyntaxKind::IdentifierName:
        case SyntaxKind::GenericName:
        case SyntaxKind::QualifiedName:
        case SyntaxKind::AliasQualifiedName:
            return true;
        default:
            return false;
    }
}

bool IsPredefinedType(SyntaxKind kind)
{
    switch (kind)
    {
        case SyntaxKind::BoolKeyword:
        case SyntaxKind::ByteKeyword:
        case SyntaxKind::SByteKeyword:
        case SyntaxKind::IntKeyword:
        case SyntaxKind::UIntKeyword:
        case SyntaxKind::ShortKeyword:
        case SyntaxKind::UShortKeyword:
        case SyntaxKind::LongKeyword:
        case SyntaxKind::ULongKeyword:
        case SyntaxKind::FloatKeyword:
        case SyntaxKind::DoubleKeyword:
        case SyntaxKind::DecimalKeyword:
        case SyntaxKind::StringKeyword:
        case SyntaxKind::CharKeyword:
        case SyntaxKind::ObjectKeyword:
        case SyntaxKind::VoidKeyword:
            return true;
        default:
            return false;
    }
}

bool IsTypeSyntax(SyntaxKind kind)
{
    switch (kind)
    {
        case SyntaxKind::ArrayType:
        case SyntaxKind::PointerType:
        case SyntaxKind::NullableType:
        case SyntaxKind::PredefinedType:
        case SyntaxKind::TupleType:
        case SyntaxKind::FunctionPointerType:
            return true;
        default:
            return IsName(kind);
    }
}

/// <summary>
/// Member declarations that can appear in global code (other than type declarations).
/// </summary>
bool IsGlobalMemberDeclaration(SyntaxKind kind)
{
    switch (kind)
    {
        case SyntaxKind::GlobalStatement:
        case SyntaxKind::FieldDeclaration:
        case SyntaxKind::MethodDeclaration:
        case SyntaxKind::PropertyDeclaration:
        case SyntaxKind::EventDeclaration:
        case SyntaxKind::EventFieldDeclaration:
            return true;
        default:
            break;
    }
    return false;
}

bool IsTypeDeclaration(SyntaxKind kind)
{
    switch (kind)
    {
        case SyntaxKind::ClassDeclaration:
        case SyntaxKind::StructDeclaration:
        case SyntaxKind::UnionDeclaration:
        case SyntaxKind::InterfaceDeclaration:
        case SyntaxKind::DelegateDeclaration:
        case SyntaxKind::EnumDeclaration:
        case SyntaxKind::RecordDeclaration:
        case SyntaxKind::RecordStructDeclaration:
        case SyntaxKind::ExtensionBlockDeclaration:
            return true;

        default:
            return false;
    }
}

bool IsNamespaceMemberDeclaration(SyntaxKind kind)
{
    return IsTypeDeclaration(kind) ||
           kind == SyntaxKind::NamespaceDeclaration ||
           kind == SyntaxKind::FileScopedNamespaceDeclaration;
}

bool IsAnyUnaryExpression(SyntaxKind token)
{
    return IsPrefixUnaryExpression(token) || IsPostfixUnaryExpression(token);
}

bool IsPrefixUnaryExpression(SyntaxKind token)
{
    return GetPrefixUnaryExpression(token) != SyntaxKind::None;
}

bool IsPrefixUnaryExpressionOperatorToken(SyntaxKind token)
{
    return GetPrefixUnaryExpression(token) != SyntaxKind::None;
}

SyntaxKind GetPrefixUnaryExpression(SyntaxKind token)
{
    switch (token)
    {
        case SyntaxKind::PlusToken:
            return SyntaxKind::UnaryPlusExpression;
        case SyntaxKind::MinusToken:
            return SyntaxKind::UnaryMinusExpression;
        case SyntaxKind::TildeToken:
            return SyntaxKind::BitwiseNotExpression;
        case SyntaxKind::ExclamationToken:
            return SyntaxKind::LogicalNotExpression;
        case SyntaxKind::PlusPlusToken:
            return SyntaxKind::PreIncrementExpression;
        case SyntaxKind::MinusMinusToken:
            return SyntaxKind::PreDecrementExpression;
        case SyntaxKind::AmpersandToken:
            return SyntaxKind::AddressOfExpression;
        case SyntaxKind::AsteriskToken:
            return SyntaxKind::PointerIndirectionExpression;
        case SyntaxKind::CaretToken:
            return SyntaxKind::IndexExpression;
        default:
            return SyntaxKind::None;
    }
}

bool IsPostfixUnaryExpression(SyntaxKind token)
{
    return GetPostfixUnaryExpression(token) != SyntaxKind::None;
}

bool IsPostfixUnaryExpressionToken(SyntaxKind token)
{
    return GetPostfixUnaryExpression(token) != SyntaxKind::None;
}

SyntaxKind GetPostfixUnaryExpression(SyntaxKind token)
{
    switch (token)
    {
        case SyntaxKind::PlusPlusToken:
            return SyntaxKind::PostIncrementExpression;
        case SyntaxKind::MinusMinusToken:
            return SyntaxKind::PostDecrementExpression;
        case SyntaxKind::ExclamationToken:
            return SyntaxKind::SuppressNullableWarningExpression;
        default:
            return SyntaxKind::None;
    }
}

bool IsIncrementOrDecrementOperator(SyntaxKind token)
{
    switch (token)
    {
        case SyntaxKind::PlusPlusToken:
        case SyntaxKind::MinusMinusToken:
            return true;
        default:
            return false;
    }
}

bool IsUnaryOperatorDeclarationToken(SyntaxKind token)
{
    return IsPrefixUnaryExpressionOperatorToken(token) ||
           token == SyntaxKind::TrueKeyword ||
           token == SyntaxKind::FalseKeyword;
}

bool IsAnyOverloadableOperator(SyntaxKind kind)
{
    return IsOverloadableBinaryOperator(kind) ||
           IsOverloadableUnaryOperator(kind) ||
           IsOverloadableCompoundAssignmentOperator(kind);
}

bool IsOverloadableBinaryOperator(SyntaxKind kind)
{
    switch (kind)
    {
        case SyntaxKind::PlusToken:
        case SyntaxKind::MinusToken:
        case SyntaxKind::AsteriskToken:
        case SyntaxKind::SlashToken:
        case SyntaxKind::PercentToken:
        case SyntaxKind::CaretToken:
        case SyntaxKind::AmpersandToken:
        case SyntaxKind::BarToken:
        case SyntaxKind::EqualsEqualsToken:
        case SyntaxKind::LessThanToken:
        case SyntaxKind::LessThanEqualsToken:
        case SyntaxKind::LessThanLessThanToken:
        case SyntaxKind::GreaterThanToken:
        case SyntaxKind::GreaterThanEqualsToken:
        case SyntaxKind::GreaterThanGreaterThanToken:
        case SyntaxKind::GreaterThanGreaterThanGreaterThanToken:
        case SyntaxKind::ExclamationEqualsToken:
            return true;
        default:
            return false;
    }
}

bool IsOverloadableUnaryOperator(SyntaxKind kind)
{
    switch (kind)
    {
        case SyntaxKind::PlusToken:
        case SyntaxKind::MinusToken:
        case SyntaxKind::TildeToken:
        case SyntaxKind::ExclamationToken:
        case SyntaxKind::PlusPlusToken:
        case SyntaxKind::MinusMinusToken:
        case SyntaxKind::TrueKeyword:
        case SyntaxKind::FalseKeyword:
            return true;
        default:
            return false;
    }
}

bool IsOverloadableCompoundAssignmentOperator(SyntaxKind kind)
{
    switch (kind)
    {
        case SyntaxKind::PlusEqualsToken:
        case SyntaxKind::MinusEqualsToken:
        case SyntaxKind::AsteriskEqualsToken:
        case SyntaxKind::SlashEqualsToken:
        case SyntaxKind::PercentEqualsToken:
        case SyntaxKind::AmpersandEqualsToken:
        case SyntaxKind::BarEqualsToken:
        case SyntaxKind::CaretEqualsToken:
        case SyntaxKind::LessThanLessThanEqualsToken:
        case SyntaxKind::GreaterThanGreaterThanEqualsToken:
        case SyntaxKind::GreaterThanGreaterThanGreaterThanEqualsToken:
            return true;
        default:
            return false;
    }
}

bool IsPrimaryFunction(SyntaxKind keyword)
{
    return GetPrimaryFunction(keyword) != SyntaxKind::None;
}

SyntaxKind GetPrimaryFunction(SyntaxKind keyword)
{
    switch (keyword)
    {
        case SyntaxKind::MakeRefKeyword:
            return SyntaxKind::MakeRefExpression;
        case SyntaxKind::RefTypeKeyword:
            return SyntaxKind::RefTypeExpression;
        case SyntaxKind::RefValueKeyword:
            return SyntaxKind::RefValueExpression;
        case SyntaxKind::CheckedKeyword:
            return SyntaxKind::CheckedExpression;
        case SyntaxKind::UncheckedKeyword:
            return SyntaxKind::UncheckedExpression;
        case SyntaxKind::DefaultKeyword:
            return SyntaxKind::DefaultExpression;
        case SyntaxKind::TypeOfKeyword:
            return SyntaxKind::TypeOfExpression;
        case SyntaxKind::SizeOfKeyword:
            return SyntaxKind::SizeOfExpression;
        default:
            return SyntaxKind::None;
    }
}

bool IsLiteralExpression(SyntaxKind token)
{
    return GetLiteralExpression(token) != SyntaxKind::None;
}

SyntaxKind GetLiteralExpression(SyntaxKind token)
{
    switch (token)
    {
        case SyntaxKind::StringLiteralToken:
            return SyntaxKind::StringLiteralExpression;
        case SyntaxKind::Utf8StringLiteralToken:
            return SyntaxKind::Utf8StringLiteralExpression;
        case SyntaxKind::SingleLineRawStringLiteralToken:
            return SyntaxKind::StringLiteralExpression;
        case SyntaxKind::Utf8SingleLineRawStringLiteralToken:
            return SyntaxKind::Utf8StringLiteralExpression;
        case SyntaxKind::MultiLineRawStringLiteralToken:
            return SyntaxKind::StringLiteralExpression;
        case SyntaxKind::Utf8MultiLineRawStringLiteralToken:
            return SyntaxKind::Utf8StringLiteralExpression;
        case SyntaxKind::CharacterLiteralToken:
            return SyntaxKind::CharacterLiteralExpression;
        case SyntaxKind::NumericLiteralToken:
            return SyntaxKind::NumericLiteralExpression;
        case SyntaxKind::NullKeyword:
            return SyntaxKind::NullLiteralExpression;
        case SyntaxKind::TrueKeyword:
            return SyntaxKind::TrueLiteralExpression;
        case SyntaxKind::FalseKeyword:
            return SyntaxKind::FalseLiteralExpression;
        case SyntaxKind::ArgListKeyword:
            return SyntaxKind::ArgListExpression;
        default:
            return SyntaxKind::None;
    }
}

bool IsInstanceExpression(SyntaxKind token)
{
    return GetInstanceExpression(token) != SyntaxKind::None;
}

SyntaxKind GetInstanceExpression(SyntaxKind token)
{
    switch (token)
    {
        case SyntaxKind::ThisKeyword:
            return SyntaxKind::ThisExpression;
        case SyntaxKind::BaseKeyword:
            return SyntaxKind::BaseExpression;
        default:
            return SyntaxKind::None;
    }
}

bool IsBinaryExpression(SyntaxKind token)
{
    return GetBinaryExpression(token) != SyntaxKind::None;
}

bool IsBinaryExpressionOperatorToken(SyntaxKind token)
{
    return GetBinaryExpression(token) != SyntaxKind::None;
}

SyntaxKind GetBinaryExpression(SyntaxKind token)
{
    switch (token)
    {
        case SyntaxKind::QuestionQuestionToken:
            return SyntaxKind::CoalesceExpression;
        case SyntaxKind::IsKeyword:
            return SyntaxKind::IsExpression;
        case SyntaxKind::AsKeyword:
            return SyntaxKind::AsExpression;
        case SyntaxKind::BarToken:
            return SyntaxKind::BitwiseOrExpression;
        case SyntaxKind::CaretToken:
            return SyntaxKind::ExclusiveOrExpression;
        case SyntaxKind::AmpersandToken:
            return SyntaxKind::BitwiseAndExpression;
        case SyntaxKind::EqualsEqualsToken:
            return SyntaxKind::EqualsExpression;
        case SyntaxKind::ExclamationEqualsToken:
            return SyntaxKind::NotEqualsExpression;
        case SyntaxKind::LessThanToken:
            return SyntaxKind::LessThanExpression;
        case SyntaxKind::LessThanEqualsToken:
            return SyntaxKind::LessThanOrEqualExpression;
        case SyntaxKind::GreaterThanToken:
            return SyntaxKind::GreaterThanExpression;
        case SyntaxKind::GreaterThanEqualsToken:
            return SyntaxKind::GreaterThanOrEqualExpression;
        case SyntaxKind::LessThanLessThanToken:
            return SyntaxKind::LeftShiftExpression;
        case SyntaxKind::GreaterThanGreaterThanToken:
            return SyntaxKind::RightShiftExpression;
        case SyntaxKind::GreaterThanGreaterThanGreaterThanToken:
            return SyntaxKind::UnsignedRightShiftExpression;
        case SyntaxKind::PlusToken:
            return SyntaxKind::AddExpression;
        case SyntaxKind::MinusToken:
            return SyntaxKind::SubtractExpression;
        case SyntaxKind::AsteriskToken:
            return SyntaxKind::MultiplyExpression;
        case SyntaxKind::SlashToken:
            return SyntaxKind::DivideExpression;
        case SyntaxKind::PercentToken:
            return SyntaxKind::ModuloExpression;
        case SyntaxKind::AmpersandAmpersandToken:
            return SyntaxKind::LogicalAndExpression;
        case SyntaxKind::BarBarToken:
            return SyntaxKind::LogicalOrExpression;
        default:
            return SyntaxKind::None;
    }
}

bool IsAssignmentExpression(SyntaxKind kind)
{
    switch (kind)
    {
        case SyntaxKind::CoalesceAssignmentExpression:
        case SyntaxKind::OrAssignmentExpression:
        case SyntaxKind::AndAssignmentExpression:
        case SyntaxKind::ExclusiveOrAssignmentExpression:
        case SyntaxKind::LeftShiftAssignmentExpression:
        case SyntaxKind::RightShiftAssignmentExpression:
        case SyntaxKind::UnsignedRightShiftAssignmentExpression:
        case SyntaxKind::AddAssignmentExpression:
        case SyntaxKind::SubtractAssignmentExpression:
        case SyntaxKind::MultiplyAssignmentExpression:
        case SyntaxKind::DivideAssignmentExpression:
        case SyntaxKind::ModuloAssignmentExpression:
        case SyntaxKind::SimpleAssignmentExpression:
            return true;
        default:
            return false;
    }
}

bool IsAssignmentExpressionOperatorToken(SyntaxKind token)
{
    switch (token)
    {
        case SyntaxKind::QuestionQuestionEqualsToken:
        case SyntaxKind::BarEqualsToken:
        case SyntaxKind::AmpersandEqualsToken:
        case SyntaxKind::CaretEqualsToken:
        case SyntaxKind::LessThanLessThanEqualsToken:
        case SyntaxKind::GreaterThanGreaterThanEqualsToken:
        case SyntaxKind::GreaterThanGreaterThanGreaterThanEqualsToken:
        case SyntaxKind::PlusEqualsToken:
        case SyntaxKind::MinusEqualsToken:
        case SyntaxKind::AsteriskEqualsToken:
        case SyntaxKind::SlashEqualsToken:
        case SyntaxKind::PercentEqualsToken:
        case SyntaxKind::EqualsToken:
            return true;
        default:
            return false;
    }
}

SyntaxKind GetAssignmentExpression(SyntaxKind token)
{
    switch (token)
    {
        case SyntaxKind::BarEqualsToken:
            return SyntaxKind::OrAssignmentExpression;
        case SyntaxKind::AmpersandEqualsToken:
            return SyntaxKind::AndAssignmentExpression;
        case SyntaxKind::CaretEqualsToken:
            return SyntaxKind::ExclusiveOrAssignmentExpression;
        case SyntaxKind::LessThanLessThanEqualsToken:
            return SyntaxKind::LeftShiftAssignmentExpression;
        case SyntaxKind::GreaterThanGreaterThanEqualsToken:
            return SyntaxKind::RightShiftAssignmentExpression;
        case SyntaxKind::GreaterThanGreaterThanGreaterThanEqualsToken:
            return SyntaxKind::UnsignedRightShiftAssignmentExpression;
        case SyntaxKind::PlusEqualsToken:
            return SyntaxKind::AddAssignmentExpression;
        case SyntaxKind::MinusEqualsToken:
            return SyntaxKind::SubtractAssignmentExpression;
        case SyntaxKind::AsteriskEqualsToken:
            return SyntaxKind::MultiplyAssignmentExpression;
        case SyntaxKind::SlashEqualsToken:
            return SyntaxKind::DivideAssignmentExpression;
        case SyntaxKind::PercentEqualsToken:
            return SyntaxKind::ModuloAssignmentExpression;
        case SyntaxKind::EqualsToken:
            return SyntaxKind::SimpleAssignmentExpression;
        case SyntaxKind::QuestionQuestionEqualsToken:
            return SyntaxKind::CoalesceAssignmentExpression;
        default:
            return SyntaxKind::None;
    }
}

SyntaxKind GetCheckStatement(SyntaxKind keyword)
{
    switch (keyword)
    {
        case SyntaxKind::CheckedKeyword:
            return SyntaxKind::CheckedStatement;
        case SyntaxKind::UncheckedKeyword:
            return SyntaxKind::UncheckedStatement;
        default:
            return SyntaxKind::None;
    }
}

SyntaxKind GetAccessorDeclarationKind(SyntaxKind keyword)
{
    switch (keyword)
    {
        case SyntaxKind::GetKeyword:
            return SyntaxKind::GetAccessorDeclaration;
        case SyntaxKind::SetKeyword:
            return SyntaxKind::SetAccessorDeclaration;
        case SyntaxKind::InitKeyword:
            return SyntaxKind::InitAccessorDeclaration;
        case SyntaxKind::AddKeyword:
            return SyntaxKind::AddAccessorDeclaration;
        case SyntaxKind::RemoveKeyword:
            return SyntaxKind::RemoveAccessorDeclaration;
        default:
            return SyntaxKind::None;
    }
}

bool IsAccessorDeclaration(SyntaxKind kind)
{
    switch (kind)
    {
        case SyntaxKind::GetAccessorDeclaration:
        case SyntaxKind::SetAccessorDeclaration:
        case SyntaxKind::InitAccessorDeclaration:
        case SyntaxKind::AddAccessorDeclaration:
        case SyntaxKind::RemoveAccessorDeclaration:
            return true;
        default:
            return false;
    }
}

bool IsAccessorDeclarationKeyword(SyntaxKind keyword)
{
    switch (keyword)
    {
        case SyntaxKind::GetKeyword:
        case SyntaxKind::SetKeyword:
        case SyntaxKind::InitKeyword:
        case SyntaxKind::AddKeyword:
        case SyntaxKind::RemoveKeyword:
            return true;
        default:
            return false;
    }
}

SyntaxKind GetSwitchLabelKind(SyntaxKind keyword)
{
    switch (keyword)
    {
        case SyntaxKind::CaseKeyword:
            return SyntaxKind::CaseSwitchLabel;
        case SyntaxKind::DefaultKeyword:
            return SyntaxKind::DefaultSwitchLabel;
        default:
            return SyntaxKind::None;
    }
}

SyntaxKind GetBaseTypeDeclarationKind(SyntaxKind kind)
{
    return kind == SyntaxKind::EnumKeyword ? SyntaxKind::EnumDeclaration : GetTypeDeclarationKind(kind);
}

SyntaxKind GetTypeDeclarationKind(SyntaxKind kind)
{
    switch (kind)
    {
        case SyntaxKind::ClassKeyword:
            return SyntaxKind::ClassDeclaration;
        case SyntaxKind::StructKeyword:
            return SyntaxKind::StructDeclaration;
        case SyntaxKind::UnionKeyword:
            return SyntaxKind::UnionDeclaration;
        case SyntaxKind::InterfaceKeyword:
            return SyntaxKind::InterfaceDeclaration;
        case SyntaxKind::RecordKeyword:
            return SyntaxKind::RecordDeclaration;
        case SyntaxKind::ExtensionKeyword:
            return SyntaxKind::ExtensionBlockDeclaration;
        default:
            return SyntaxKind::None;
    }
}

SyntaxKind GetKeywordKind(Text text)
{
    // switch (text) { case "...": ... }, as a table lookup.
    static const KeywordTable table({
        {u"bool", SyntaxKind::BoolKeyword},
        {u"byte", SyntaxKind::ByteKeyword},
        {u"sbyte", SyntaxKind::SByteKeyword},
        {u"short", SyntaxKind::ShortKeyword},
        {u"ushort", SyntaxKind::UShortKeyword},
        {u"int", SyntaxKind::IntKeyword},
        {u"uint", SyntaxKind::UIntKeyword},
        {u"long", SyntaxKind::LongKeyword},
        {u"ulong", SyntaxKind::ULongKeyword},
        {u"double", SyntaxKind::DoubleKeyword},
        {u"float", SyntaxKind::FloatKeyword},
        {u"decimal", SyntaxKind::DecimalKeyword},
        {u"string", SyntaxKind::StringKeyword},
        {u"char", SyntaxKind::CharKeyword},
        {u"void", SyntaxKind::VoidKeyword},
        {u"object", SyntaxKind::ObjectKeyword},
        {u"typeof", SyntaxKind::TypeOfKeyword},
        {u"sizeof", SyntaxKind::SizeOfKeyword},
        {u"null", SyntaxKind::NullKeyword},
        {u"true", SyntaxKind::TrueKeyword},
        {u"false", SyntaxKind::FalseKeyword},
        {u"if", SyntaxKind::IfKeyword},
        {u"else", SyntaxKind::ElseKeyword},
        {u"while", SyntaxKind::WhileKeyword},
        {u"for", SyntaxKind::ForKeyword},
        {u"foreach", SyntaxKind::ForEachKeyword},
        {u"do", SyntaxKind::DoKeyword},
        {u"switch", SyntaxKind::SwitchKeyword},
        {u"case", SyntaxKind::CaseKeyword},
        {u"default", SyntaxKind::DefaultKeyword},
        {u"lock", SyntaxKind::LockKeyword},
        {u"try", SyntaxKind::TryKeyword},
        {u"throw", SyntaxKind::ThrowKeyword},
        {u"catch", SyntaxKind::CatchKeyword},
        {u"finally", SyntaxKind::FinallyKeyword},
        {u"goto", SyntaxKind::GotoKeyword},
        {u"break", SyntaxKind::BreakKeyword},
        {u"continue", SyntaxKind::ContinueKeyword},
        {u"return", SyntaxKind::ReturnKeyword},
        {u"public", SyntaxKind::PublicKeyword},
        {u"private", SyntaxKind::PrivateKeyword},
        {u"internal", SyntaxKind::InternalKeyword},
        {u"protected", SyntaxKind::ProtectedKeyword},
        {u"static", SyntaxKind::StaticKeyword},
        {u"readonly", SyntaxKind::ReadOnlyKeyword},
        {u"sealed", SyntaxKind::SealedKeyword},
        {u"const", SyntaxKind::ConstKeyword},
        {u"fixed", SyntaxKind::FixedKeyword},
        {u"stackalloc", SyntaxKind::StackAllocKeyword},
        {u"volatile", SyntaxKind::VolatileKeyword},
        {u"new", SyntaxKind::NewKeyword},
        {u"override", SyntaxKind::OverrideKeyword},
        {u"abstract", SyntaxKind::AbstractKeyword},
        {u"virtual", SyntaxKind::VirtualKeyword},
        {u"event", SyntaxKind::EventKeyword},
        {u"extern", SyntaxKind::ExternKeyword},
        {u"ref", SyntaxKind::RefKeyword},
        {u"out", SyntaxKind::OutKeyword},
        {u"in", SyntaxKind::InKeyword},
        {u"is", SyntaxKind::IsKeyword},
        {u"as", SyntaxKind::AsKeyword},
        {u"params", SyntaxKind::ParamsKeyword},
        {u"__arglist", SyntaxKind::ArgListKeyword},
        {u"__makeref", SyntaxKind::MakeRefKeyword},
        {u"__reftype", SyntaxKind::RefTypeKeyword},
        {u"__refvalue", SyntaxKind::RefValueKeyword},
        {u"this", SyntaxKind::ThisKeyword},
        {u"base", SyntaxKind::BaseKeyword},
        {u"namespace", SyntaxKind::NamespaceKeyword},
        {u"using", SyntaxKind::UsingKeyword},
        {u"class", SyntaxKind::ClassKeyword},
        {u"struct", SyntaxKind::StructKeyword},
        {u"interface", SyntaxKind::InterfaceKeyword},
        {u"enum", SyntaxKind::EnumKeyword},
        {u"delegate", SyntaxKind::DelegateKeyword},
        {u"checked", SyntaxKind::CheckedKeyword},
        {u"unchecked", SyntaxKind::UncheckedKeyword},
        {u"unsafe", SyntaxKind::UnsafeKeyword},
        {u"operator", SyntaxKind::OperatorKeyword},
        {u"implicit", SyntaxKind::ImplicitKeyword},
        {u"explicit", SyntaxKind::ExplicitKeyword},
    });
    return table.Find(text);
}

// GetOperatorKind / IsCheckedOperator (metadata operator names): not ported, semantic-layer only.

SyntaxKind GetPreprocessorKeywordKind(Text text)
{
    // switch (text) { case "...": ... }, as a table lookup.
    static const KeywordTable table({
        {u"true", SyntaxKind::TrueKeyword},
        {u"false", SyntaxKind::FalseKeyword},
        {u"default", SyntaxKind::DefaultKeyword},
        {u"if", SyntaxKind::IfKeyword},
        {u"else", SyntaxKind::ElseKeyword},
        {u"elif", SyntaxKind::ElifKeyword},
        {u"endif", SyntaxKind::EndIfKeyword},
        {u"region", SyntaxKind::RegionKeyword},
        {u"endregion", SyntaxKind::EndRegionKeyword},
        {u"define", SyntaxKind::DefineKeyword},
        {u"undef", SyntaxKind::UndefKeyword},
        {u"warning", SyntaxKind::WarningKeyword},
        {u"error", SyntaxKind::ErrorKeyword},
        {u"line", SyntaxKind::LineKeyword},
        {u"pragma", SyntaxKind::PragmaKeyword},
        {u"hidden", SyntaxKind::HiddenKeyword},
        {u"checksum", SyntaxKind::ChecksumKeyword},
        {u"disable", SyntaxKind::DisableKeyword},
        {u"restore", SyntaxKind::RestoreKeyword},
        {u"r", SyntaxKind::ReferenceKeyword},
        {u"load", SyntaxKind::LoadKeyword},
        {u"nullable", SyntaxKind::NullableKeyword},
        {u"enable", SyntaxKind::EnableKeyword},
        {u"warnings", SyntaxKind::WarningsKeyword},
        {u"annotations", SyntaxKind::AnnotationsKeyword},
    });
    return table.Find(text);
}

std::vector<SyntaxKind> GetContextualKeywordKinds()
{
    std::vector<SyntaxKind> result;
    for (int i = static_cast<int>(SyntaxKind::YieldKeyword); i <= static_cast<int>(SyntaxKind::UnionKeyword); i++)
    {
        // 8441 corresponds to a deleted kind (DataKeyword) that was previously shipped.
        if (i != 8441)
        {
            result.push_back(static_cast<SyntaxKind>(i));
        }
    }
    return result;
}

bool IsContextualKeyword(SyntaxKind kind)
{
    switch (kind)
    {
        case SyntaxKind::YieldKeyword:
        case SyntaxKind::PartialKeyword:
        case SyntaxKind::FromKeyword:
        case SyntaxKind::GroupKeyword:
        case SyntaxKind::JoinKeyword:
        case SyntaxKind::IntoKeyword:
        case SyntaxKind::LetKeyword:
        case SyntaxKind::ByKeyword:
        case SyntaxKind::WhereKeyword:
        case SyntaxKind::SelectKeyword:
        case SyntaxKind::GetKeyword:
        case SyntaxKind::SetKeyword:
        case SyntaxKind::AddKeyword:
        case SyntaxKind::RemoveKeyword:
        case SyntaxKind::OrderByKeyword:
        case SyntaxKind::AliasKeyword:
        case SyntaxKind::OnKeyword:
        case SyntaxKind::EqualsKeyword:
        case SyntaxKind::AscendingKeyword:
        case SyntaxKind::DescendingKeyword:
        case SyntaxKind::AssemblyKeyword:
        case SyntaxKind::ModuleKeyword:
        case SyntaxKind::TypeKeyword:
        case SyntaxKind::GlobalKeyword:
        case SyntaxKind::FieldKeyword:
        case SyntaxKind::MethodKeyword:
        case SyntaxKind::ParamKeyword:
        case SyntaxKind::PropertyKeyword:
        case SyntaxKind::TypeVarKeyword:
        case SyntaxKind::NameOfKeyword:
        case SyntaxKind::AsyncKeyword:
        case SyntaxKind::AwaitKeyword:
        case SyntaxKind::WhenKeyword:
        case SyntaxKind::UnderscoreToken:
        case SyntaxKind::VarKeyword:
        case SyntaxKind::OrKeyword:
        case SyntaxKind::AndKeyword:
        case SyntaxKind::NotKeyword:
        case SyntaxKind::WithKeyword:
        case SyntaxKind::InitKeyword:
        case SyntaxKind::RecordKeyword:
        case SyntaxKind::ManagedKeyword:
        case SyntaxKind::UnmanagedKeyword:
        case SyntaxKind::RequiredKeyword:
        case SyntaxKind::ScopedKeyword:
        case SyntaxKind::FileKeyword:
        case SyntaxKind::AllowsKeyword:
        case SyntaxKind::ExtensionKeyword:
        case SyntaxKind::UnionKeyword:
            return true;
        default:
            return false;
    }
}

bool IsQueryContextualKeyword(SyntaxKind kind)
{
    switch (kind)
    {
        case SyntaxKind::FromKeyword:
        case SyntaxKind::WhereKeyword:
        case SyntaxKind::SelectKeyword:
        case SyntaxKind::GroupKeyword:
        case SyntaxKind::IntoKeyword:
        case SyntaxKind::OrderByKeyword:
        case SyntaxKind::JoinKeyword:
        case SyntaxKind::LetKeyword:
        case SyntaxKind::OnKeyword:
        case SyntaxKind::EqualsKeyword:
        case SyntaxKind::ByKeyword:
        case SyntaxKind::AscendingKeyword:
        case SyntaxKind::DescendingKeyword:
            return true;
        default:
            return false;
    }
}

SyntaxKind GetContextualKeywordKind(Text text)
{
    // switch (text) { case "...": ... }, as a table lookup.
    static const KeywordTable table({
        {u"yield", SyntaxKind::YieldKeyword},
        {u"partial", SyntaxKind::PartialKeyword},
        {u"from", SyntaxKind::FromKeyword},
        {u"group", SyntaxKind::GroupKeyword},
        {u"join", SyntaxKind::JoinKeyword},
        {u"into", SyntaxKind::IntoKeyword},
        {u"let", SyntaxKind::LetKeyword},
        {u"by", SyntaxKind::ByKeyword},
        {u"where", SyntaxKind::WhereKeyword},
        {u"select", SyntaxKind::SelectKeyword},
        {u"get", SyntaxKind::GetKeyword},
        {u"set", SyntaxKind::SetKeyword},
        {u"add", SyntaxKind::AddKeyword},
        {u"remove", SyntaxKind::RemoveKeyword},
        {u"orderby", SyntaxKind::OrderByKeyword},
        {u"alias", SyntaxKind::AliasKeyword},
        {u"on", SyntaxKind::OnKeyword},
        {u"equals", SyntaxKind::EqualsKeyword},
        {u"ascending", SyntaxKind::AscendingKeyword},
        {u"descending", SyntaxKind::DescendingKeyword},
        {u"assembly", SyntaxKind::AssemblyKeyword},
        {u"module", SyntaxKind::ModuleKeyword},
        {u"type", SyntaxKind::TypeKeyword},
        {u"field", SyntaxKind::FieldKeyword},
        {u"method", SyntaxKind::MethodKeyword},
        {u"param", SyntaxKind::ParamKeyword},
        {u"property", SyntaxKind::PropertyKeyword},
        {u"typevar", SyntaxKind::TypeVarKeyword},
        {u"global", SyntaxKind::GlobalKeyword},
        {u"async", SyntaxKind::AsyncKeyword},
        {u"await", SyntaxKind::AwaitKeyword},
        {u"when", SyntaxKind::WhenKeyword},
        {u"nameof", SyntaxKind::NameOfKeyword},
        {u"_", SyntaxKind::UnderscoreToken},
        {u"var", SyntaxKind::VarKeyword},
        {u"and", SyntaxKind::AndKeyword},
        {u"or", SyntaxKind::OrKeyword},
        {u"not", SyntaxKind::NotKeyword},
        {u"with", SyntaxKind::WithKeyword},
        {u"init", SyntaxKind::InitKeyword},
        {u"record", SyntaxKind::RecordKeyword},
        {u"managed", SyntaxKind::ManagedKeyword},
        {u"unmanaged", SyntaxKind::UnmanagedKeyword},
        {u"required", SyntaxKind::RequiredKeyword},
        {u"scoped", SyntaxKind::ScopedKeyword},
        {u"file", SyntaxKind::FileKeyword},
        {u"allows", SyntaxKind::AllowsKeyword},
        {u"extension", SyntaxKind::ExtensionKeyword},
        {u"union", SyntaxKind::UnionKeyword},
    });
    return table.Find(text);
}

Text GetText(SyntaxKind kind)
{
    switch (kind)
    {
        case SyntaxKind::TildeToken:
            return u"~";
        case SyntaxKind::ExclamationToken:
            return u"!";
        case SyntaxKind::DollarToken:
            return u"$";
        case SyntaxKind::PercentToken:
            return u"%";
        case SyntaxKind::CaretToken:
            return u"^";
        case SyntaxKind::AmpersandToken:
            return u"&";
        case SyntaxKind::AsteriskToken:
            return u"*";
        case SyntaxKind::OpenParenToken:
            return u"(";
        case SyntaxKind::CloseParenToken:
            return u")";
        case SyntaxKind::MinusToken:
            return u"-";
        case SyntaxKind::PlusToken:
            return u"+";
        case SyntaxKind::EqualsToken:
            return u"=";
        case SyntaxKind::OpenBraceToken:
            return u"{";
        case SyntaxKind::CloseBraceToken:
            return u"}";
        case SyntaxKind::OpenBracketToken:
            return u"[";
        case SyntaxKind::CloseBracketToken:
            return u"]";
        case SyntaxKind::BarToken:
            return u"|";
        case SyntaxKind::BackslashToken:
            return u"\\";
        case SyntaxKind::ColonToken:
            return u":";
        case SyntaxKind::SemicolonToken:
            return u";";
        case SyntaxKind::DoubleQuoteToken:
            return u"\"";
        case SyntaxKind::SingleQuoteToken:
            return u"'";
        case SyntaxKind::LessThanToken:
            return u"<";
        case SyntaxKind::CommaToken:
            return u",";
        case SyntaxKind::GreaterThanToken:
            return u">";
        case SyntaxKind::DotToken:
            return u".";
        case SyntaxKind::QuestionToken:
            return u"?";
        case SyntaxKind::HashToken:
            return u"#";
        case SyntaxKind::SlashToken:
            return u"/";
        case SyntaxKind::SlashGreaterThanToken:
            return u"/>";
        case SyntaxKind::LessThanSlashToken:
            return u"</";
        case SyntaxKind::XmlCommentStartToken:
            return u"<!--";
        case SyntaxKind::XmlCommentEndToken:
            return u"-->";
        case SyntaxKind::XmlCDataStartToken:
            return u"<![CDATA[";
        case SyntaxKind::XmlCDataEndToken:
            return u"]]>";
        case SyntaxKind::XmlProcessingInstructionStartToken:
            return u"<?";
        case SyntaxKind::XmlProcessingInstructionEndToken:
            return u"?>";

        // compound
        case SyntaxKind::BarBarToken:
            return u"||";
        case SyntaxKind::AmpersandAmpersandToken:
            return u"&&";
        case SyntaxKind::MinusMinusToken:
            return u"--";
        case SyntaxKind::PlusPlusToken:
            return u"++";
        case SyntaxKind::ColonColonToken:
            return u"::";
        case SyntaxKind::QuestionQuestionToken:
            return u"??";
        case SyntaxKind::MinusGreaterThanToken:
            return u"->";
        case SyntaxKind::ExclamationEqualsToken:
            return u"!=";
        case SyntaxKind::EqualsEqualsToken:
            return u"==";
        case SyntaxKind::EqualsGreaterThanToken:
            return u"=>";
        case SyntaxKind::LessThanEqualsToken:
            return u"<=";
        case SyntaxKind::LessThanLessThanToken:
            return u"<<";
        case SyntaxKind::LessThanLessThanEqualsToken:
            return u"<<=";
        case SyntaxKind::GreaterThanEqualsToken:
            return u">=";
        case SyntaxKind::GreaterThanGreaterThanToken:
            return u">>";
        case SyntaxKind::GreaterThanGreaterThanEqualsToken:
            return u">>=";
        case SyntaxKind::GreaterThanGreaterThanGreaterThanToken:
            return u">>>";
        case SyntaxKind::GreaterThanGreaterThanGreaterThanEqualsToken:
            return u">>>=";
        case SyntaxKind::SlashEqualsToken:
            return u"/=";
        case SyntaxKind::AsteriskEqualsToken:
            return u"*=";
        case SyntaxKind::BarEqualsToken:
            return u"|=";
        case SyntaxKind::AmpersandEqualsToken:
            return u"&=";
        case SyntaxKind::PlusEqualsToken:
            return u"+=";
        case SyntaxKind::MinusEqualsToken:
            return u"-=";
        case SyntaxKind::CaretEqualsToken:
            return u"^=";
        case SyntaxKind::PercentEqualsToken:
            return u"%=";
        case SyntaxKind::QuestionQuestionEqualsToken:
            return u"?\?=";  // (\? keeps it from being a trigraph)
        case SyntaxKind::DotDotToken:
            return u"..";

        // Keywords
        case SyntaxKind::BoolKeyword:
            return u"bool";
        case SyntaxKind::ByteKeyword:
            return u"byte";
        case SyntaxKind::SByteKeyword:
            return u"sbyte";
        case SyntaxKind::ShortKeyword:
            return u"short";
        case SyntaxKind::UShortKeyword:
            return u"ushort";
        case SyntaxKind::IntKeyword:
            return u"int";
        case SyntaxKind::UIntKeyword:
            return u"uint";
        case SyntaxKind::LongKeyword:
            return u"long";
        case SyntaxKind::ULongKeyword:
            return u"ulong";
        case SyntaxKind::DoubleKeyword:
            return u"double";
        case SyntaxKind::FloatKeyword:
            return u"float";
        case SyntaxKind::DecimalKeyword:
            return u"decimal";
        case SyntaxKind::StringKeyword:
            return u"string";
        case SyntaxKind::CharKeyword:
            return u"char";
        case SyntaxKind::VoidKeyword:
            return u"void";
        case SyntaxKind::ObjectKeyword:
            return u"object";
        case SyntaxKind::TypeOfKeyword:
            return u"typeof";
        case SyntaxKind::SizeOfKeyword:
            return u"sizeof";
        case SyntaxKind::NullKeyword:
            return u"null";
        case SyntaxKind::TrueKeyword:
            return u"true";
        case SyntaxKind::FalseKeyword:
            return u"false";
        case SyntaxKind::IfKeyword:
            return u"if";
        case SyntaxKind::ElseKeyword:
            return u"else";
        case SyntaxKind::WhileKeyword:
            return u"while";
        case SyntaxKind::ForKeyword:
            return u"for";
        case SyntaxKind::ForEachKeyword:
            return u"foreach";
        case SyntaxKind::DoKeyword:
            return u"do";
        case SyntaxKind::SwitchKeyword:
            return u"switch";
        case SyntaxKind::CaseKeyword:
            return u"case";
        case SyntaxKind::DefaultKeyword:
            return u"default";
        case SyntaxKind::TryKeyword:
            return u"try";
        case SyntaxKind::CatchKeyword:
            return u"catch";
        case SyntaxKind::FinallyKeyword:
            return u"finally";
        case SyntaxKind::LockKeyword:
            return u"lock";
        case SyntaxKind::GotoKeyword:
            return u"goto";
        case SyntaxKind::BreakKeyword:
            return u"break";
        case SyntaxKind::ContinueKeyword:
            return u"continue";
        case SyntaxKind::ReturnKeyword:
            return u"return";
        case SyntaxKind::ThrowKeyword:
            return u"throw";
        case SyntaxKind::PublicKeyword:
            return u"public";
        case SyntaxKind::PrivateKeyword:
            return u"private";
        case SyntaxKind::InternalKeyword:
            return u"internal";
        case SyntaxKind::ProtectedKeyword:
            return u"protected";
        case SyntaxKind::StaticKeyword:
            return u"static";
        case SyntaxKind::ReadOnlyKeyword:
            return u"readonly";
        case SyntaxKind::SealedKeyword:
            return u"sealed";
        case SyntaxKind::ConstKeyword:
            return u"const";
        case SyntaxKind::FixedKeyword:
            return u"fixed";
        case SyntaxKind::StackAllocKeyword:
            return u"stackalloc";
        case SyntaxKind::VolatileKeyword:
            return u"volatile";
        case SyntaxKind::NewKeyword:
            return u"new";
        case SyntaxKind::OverrideKeyword:
            return u"override";
        case SyntaxKind::AbstractKeyword:
            return u"abstract";
        case SyntaxKind::VirtualKeyword:
            return u"virtual";
        case SyntaxKind::EventKeyword:
            return u"event";
        case SyntaxKind::ExternKeyword:
            return u"extern";
        case SyntaxKind::RefKeyword:
            return u"ref";
        case SyntaxKind::OutKeyword:
            return u"out";
        case SyntaxKind::InKeyword:
            return u"in";
        case SyntaxKind::IsKeyword:
            return u"is";
        case SyntaxKind::AsKeyword:
            return u"as";
        case SyntaxKind::ParamsKeyword:
            return u"params";
        case SyntaxKind::ArgListKeyword:
            return u"__arglist";
        case SyntaxKind::MakeRefKeyword:
            return u"__makeref";
        case SyntaxKind::RefTypeKeyword:
            return u"__reftype";
        case SyntaxKind::RefValueKeyword:
            return u"__refvalue";
        case SyntaxKind::ThisKeyword:
            return u"this";
        case SyntaxKind::BaseKeyword:
            return u"base";
        case SyntaxKind::NamespaceKeyword:
            return u"namespace";
        case SyntaxKind::UsingKeyword:
            return u"using";
        case SyntaxKind::ClassKeyword:
            return u"class";
        case SyntaxKind::StructKeyword:
            return u"struct";
        case SyntaxKind::InterfaceKeyword:
            return u"interface";
        case SyntaxKind::EnumKeyword:
            return u"enum";
        case SyntaxKind::DelegateKeyword:
            return u"delegate";
        case SyntaxKind::CheckedKeyword:
            return u"checked";
        case SyntaxKind::UncheckedKeyword:
            return u"unchecked";
        case SyntaxKind::UnsafeKeyword:
            return u"unsafe";
        case SyntaxKind::OperatorKeyword:
            return u"operator";
        case SyntaxKind::ImplicitKeyword:
            return u"implicit";
        case SyntaxKind::ExplicitKeyword:
            return u"explicit";
        case SyntaxKind::ElifKeyword:
            return u"elif";
        case SyntaxKind::EndIfKeyword:
            return u"endif";
        case SyntaxKind::RegionKeyword:
            return u"region";
        case SyntaxKind::EndRegionKeyword:
            return u"endregion";
        case SyntaxKind::DefineKeyword:
            return u"define";
        case SyntaxKind::UndefKeyword:
            return u"undef";
        case SyntaxKind::WarningKeyword:
            return u"warning";
        case SyntaxKind::ErrorKeyword:
            return u"error";
        case SyntaxKind::LineKeyword:
            return u"line";
        case SyntaxKind::PragmaKeyword:
            return u"pragma";
        case SyntaxKind::HiddenKeyword:
            return u"hidden";
        case SyntaxKind::ChecksumKeyword:
            return u"checksum";
        case SyntaxKind::DisableKeyword:
            return u"disable";
        case SyntaxKind::RestoreKeyword:
            return u"restore";
        case SyntaxKind::ReferenceKeyword:
            return u"r";
        case SyntaxKind::LoadKeyword:
            return u"load";
        case SyntaxKind::NullableKeyword:
            return u"nullable";
        case SyntaxKind::EnableKeyword:
            return u"enable";
        case SyntaxKind::WarningsKeyword:
            return u"warnings";
        case SyntaxKind::AnnotationsKeyword:
            return u"annotations";

        // contextual keywords
        case SyntaxKind::YieldKeyword:
            return u"yield";
        case SyntaxKind::PartialKeyword:
            return u"partial";
        case SyntaxKind::FromKeyword:
            return u"from";
        case SyntaxKind::GroupKeyword:
            return u"group";
        case SyntaxKind::JoinKeyword:
            return u"join";
        case SyntaxKind::IntoKeyword:
            return u"into";
        case SyntaxKind::LetKeyword:
            return u"let";
        case SyntaxKind::ByKeyword:
            return u"by";
        case SyntaxKind::WhereKeyword:
            return u"where";
        case SyntaxKind::SelectKeyword:
            return u"select";
        case SyntaxKind::GetKeyword:
            return u"get";
        case SyntaxKind::SetKeyword:
            return u"set";
        case SyntaxKind::AddKeyword:
            return u"add";
        case SyntaxKind::RemoveKeyword:
            return u"remove";
        case SyntaxKind::OrderByKeyword:
            return u"orderby";
        case SyntaxKind::AliasKeyword:
            return u"alias";
        case SyntaxKind::OnKeyword:
            return u"on";
        case SyntaxKind::EqualsKeyword:
            return u"equals";
        case SyntaxKind::AscendingKeyword:
            return u"ascending";
        case SyntaxKind::DescendingKeyword:
            return u"descending";
        case SyntaxKind::AssemblyKeyword:
            return u"assembly";
        case SyntaxKind::ModuleKeyword:
            return u"module";
        case SyntaxKind::TypeKeyword:
            return u"type";
        case SyntaxKind::FieldKeyword:
            return u"field";
        case SyntaxKind::MethodKeyword:
            return u"method";
        case SyntaxKind::ParamKeyword:
            return u"param";
        case SyntaxKind::PropertyKeyword:
            return u"property";
        case SyntaxKind::TypeVarKeyword:
            return u"typevar";
        case SyntaxKind::GlobalKeyword:
            return u"global";
        case SyntaxKind::NameOfKeyword:
            return u"nameof";
        case SyntaxKind::AsyncKeyword:
            return u"async";
        case SyntaxKind::AwaitKeyword:
            return u"await";
        case SyntaxKind::WhenKeyword:
            return u"when";
        case SyntaxKind::InterpolatedStringStartToken:
            return u"$\"";
        case SyntaxKind::InterpolatedStringEndToken:
            return u"\"";
        case SyntaxKind::InterpolatedVerbatimStringStartToken:
            return u"$@\"";
        case SyntaxKind::UnderscoreToken:
            return u"_";
        case SyntaxKind::VarKeyword:
            return u"var";
        case SyntaxKind::AndKeyword:
            return u"and";
        case SyntaxKind::OrKeyword:
            return u"or";
        case SyntaxKind::NotKeyword:
            return u"not";
        case SyntaxKind::WithKeyword:
            return u"with";
        case SyntaxKind::InitKeyword:
            return u"init";
        case SyntaxKind::RecordKeyword:
            return u"record";
        case SyntaxKind::ManagedKeyword:
            return u"managed";
        case SyntaxKind::UnmanagedKeyword:
            return u"unmanaged";
        case SyntaxKind::RequiredKeyword:
            return u"required";
        case SyntaxKind::ScopedKeyword:
            return u"scoped";
        case SyntaxKind::FileKeyword:
            return u"file";
        case SyntaxKind::AllowsKeyword:
            return u"allows";
        case SyntaxKind::ExtensionKeyword:
            return u"extension";
        case SyntaxKind::UnionKeyword:
            return u"union";
        default:
            return Text();
    }
}

bool IsTypeParameterVarianceKeyword(SyntaxKind kind)
{
    return kind == SyntaxKind::OutKeyword || kind == SyntaxKind::InKeyword;
}

bool IsDocumentationCommentTrivia(SyntaxKind kind)
{
    return kind == SyntaxKind::SingleLineDocumentationCommentTrivia ||
        kind == SyntaxKind::MultiLineDocumentationCommentTrivia;
}

// ---- Parser/CharacterInfo.cs ------------------------------------------------------------------------------


/// <summary>
/// Returns true if the Unicode character is a hexadecimal digit.
/// </summary>
bool IsHexDigit(char16_t c)
{
    return (c >= u'0' && c <= u'9') ||
           (c >= u'A' && c <= u'F') ||
           (c >= u'a' && c <= u'f');
}

/// <summary>
/// Returns true if the Unicode character is a binary (0-1) digit.
/// </summary>
bool IsBinaryDigit(char16_t c)
{
    return (c == u'0') | (c == u'1');
}

/// <summary>
/// Returns true if the Unicode character is a decimal digit.
/// </summary>
bool IsDecDigit(char16_t c)
{
    return c >= u'0' && c <= u'9';
}

/// <summary>
/// Returns the value of a hexadecimal Unicode character.
/// </summary>
int HexValue(char16_t c)
{
    // Debug.Assert(IsHexDigit(c));
    return (c >= u'0' && c <= u'9') ? c - u'0' : (c & 0xdf) - u'A' + 10;
}

/// <summary>
/// Returns the value of a binary Unicode character.
/// </summary>
int BinaryValue(char16_t c)
{
    // Debug.Assert(IsBinaryDigit(c));
    return c - u'0';
}

/// <summary>
/// Returns the value of a decimal Unicode character.
/// </summary>
int DecValue(char16_t c)
{
    // Debug.Assert(IsDecDigit(c));
    return c - u'0';
}

// UnicodeCategory numbering (System.Globalization.UnicodeCategory), as GetUnicodeCategory returns it.
namespace {
enum UnicodeCategory : int {
    UppercaseLetter = 0,
    LowercaseLetter = 1,
    TitlecaseLetter = 2,
    ModifierLetter = 3,
    OtherLetter = 4,
    NonSpacingMark = 5,
    SpacingCombiningMark = 6,
    EnclosingMark = 7,
    DecimalDigitNumber = 8,
    LetterNumber = 9,
    OtherNumber = 10,
    SpaceSeparator = 11,
    LineSeparator = 12,
    ParagraphSeparator = 13,
    Control = 14,
    Format = 15,
    Surrogate = 16,
    PrivateUse = 17,
    ConnectorPunctuation = 18,
};
}  // namespace

/// <summary>
/// Returns true if the Unicode character represents a whitespace.
/// </summary>
bool IsWhitespace(char16_t ch)
{
    // whitespace:
    //   Any character with Unicode class Zs
    //   Horizontal tab character (U+0009)
    //   Vertical tab character (U+000B)
    //   Form feed character (U+000C)

    // Space and no-break space are the only space separators (Zs) in ASCII range

    return ch == u' '
        || ch == u'\t'
        || ch == u'\v'
        || ch == u'\f'
        || ch == u' ' // NO-BREAK SPACE
        // The native compiler recognized both the byte-order marker '﻿' and ^Z '\u001A' as whitespace
        // (see CharacterInfo.cs).
        || ch == u'﻿'
        || ch == u'\u001A'
        || (ch > 255 && GetUnicodeCategory(ch) == SpaceSeparator);
}

/// <summary>
/// Returns true if the Unicode character is a newline character.
/// </summary>
bool IsNewLine(char16_t ch)
{
    // new-line-character:
    //   Carriage return character (U+000D)
    //   Line feed character (U+000A)
    //   Next line character (U+0085)
    //   Line separator character (U+2028)
    //   Paragraph separator character (U+2029)

    return ch == u'\r'
        || ch == u'\n'
        || ch == u'\u0085'
        || ch == u' '
        || ch == u' ';
}

/// <summary>
/// Returns true if the two Unicode characters are a CR LF pair (Roslyn's SyntaxFacts.IsNewLine(char, char),
/// in the Workspaces/Core utilities; used by the lexer's trivia scanning).
/// </summary>
bool IsNewLine(char16_t ch1, char16_t ch2)
{
    return ch1 == u'\r' && ch2 == u'\n';
}

/// <summary>
/// Returns true if the Unicode character can be the starting character of a C# identifier.
/// </summary>
bool IsIdentifierStartCharacter(char16_t ch)
{
    // UnicodeCharacterUtilities.IsIdentifierStartCharacter:
    // identifier-start-character:
    //   letter-character
    //   _ (the underscore character U+005F)

    if (ch < u'a') // 'a'
    {
        if (ch < u'A') // 'A'
        {
            return false;
        }

        return ch <= u'Z'  // 'Z'
            || ch == u'_'; // '_'
    }

    if (ch <= u'z') // 'z'
    {
        return true;
    }

    if (ch <= u'\u007F') // max ASCII
    {
        return false;
    }

    return IsLetterChar(GetUnicodeCategory(ch));
}

/// <summary>
/// Returns true if the Unicode character can be a part of a C# identifier.
/// </summary>
bool IsIdentifierPartCharacter(char16_t ch)
{
    // UnicodeCharacterUtilities.IsIdentifierPartCharacter:
    // identifier-part-character:
    //   letter-character
    //   decimal-digit-character
    //   connecting-character
    //   combining-character
    //   formatting-character

    if (ch < u'a') // 'a'
    {
        if (ch < u'A') // 'A'
        {
            return ch >= u'0'  // '0'
                && ch <= u'9'; // '9'
        }

        return ch <= u'Z'  // 'Z'
            || ch == u'_'; // '_'
    }

    if (ch <= u'z') // 'z'
    {
        return true;
    }

    if (ch <= u'\u007F') // max ASCII
    {
        return false;
    }

    int cat = GetUnicodeCategory(ch);
    return IsLetterChar(cat)
        || IsDecimalDigitChar(cat)
        || IsConnectingChar(cat)
        || IsCombiningChar(cat)
        || IsFormattingChar(cat);
}

/// <summary>
/// Check that the name is a valid identifier.
/// </summary>
bool IsValidIdentifier(Text name)
{
    // UnicodeCharacterUtilities.IsValidIdentifier (null and empty are both "empty" here).
    if (name.empty())
    {
        return false;
    }

    if (!IsIdentifierStartCharacter(name[0]))
    {
        return false;
    }

    size_t nameLength = name.size();
    for (size_t i = 1; i < nameLength; i++) //NB: start at 1
    {
        if (!IsIdentifierPartCharacter(name[i]))
        {
            return false;
        }
    }

    return true;
}

/// <summary>
/// Spec section 2.4.2 says that identifiers are compared without regard
/// to leading "@" characters or unicode formatting characters.
/// </summary>
bool ContainsDroppedIdentifierCharacters(Text name)
{
    if (name.empty())
    {
        return false;
    }
    if (name[0] == u'@')
    {
        return true;
    }

    size_t nameLength = name.size();
    for (size_t i = 0; i < nameLength; i++)
    {
        if (IsFormattingChar(name[i]))
        {
            return true;
        }
    }

    return false;
}

bool IsNonAsciiQuotationMark(char16_t ch)
{
    // CONSIDER: There are others:
    // http://en.wikipedia.org/wiki/Quotation_mark_glyphs#Quotation_marks_in_Unicode
    switch (ch)
    {
        case u'‘': //LEFT SINGLE QUOTATION MARK
        case u'’': //RIGHT SINGLE QUOTATION MARK
            return true;
        case u'“': //LEFT DOUBLE QUOTATION MARK
        case u'”': //RIGHT DOUBLE QUOTATION MARK
            return true;
        default:
            return false;
    }
}

// ---- Core's UnicodeCharacterUtilities.cs (the category tests; private there, public here) -----------------

bool IsLetterChar(int cat)
{
    // letter-character:
    //   A Unicode character of classes Lu, Ll, Lt, Lm, Lo, or Nl
    //   A Unicode-escape-sequence representing a character of classes Lu, Ll, Lt, Lm, Lo, or Nl

    switch (cat)
    {
        case UppercaseLetter:
        case LowercaseLetter:
        case TitlecaseLetter:
        case ModifierLetter:
        case OtherLetter:
        case LetterNumber:
            return true;
        default:
            break;
    }

    return false;
}

bool IsCombiningChar(int cat)
{
    // combining-character:
    //   A Unicode character of classes Mn or Mc
    //   A Unicode-escape-sequence representing a character of classes Mn or Mc

    switch (cat)
    {
        case NonSpacingMark:
        case SpacingCombiningMark:
            return true;
        default:
            break;
    }

    return false;
}

bool IsDecimalDigitChar(int cat)
{
    // decimal-digit-character:
    //   A Unicode character of the class Nd
    //   A unicode-escape-sequence representing a character of the class Nd

    return cat == DecimalDigitNumber;
}

bool IsConnectingChar(int cat)
{
    // connecting-character:
    //   A Unicode character of the class Pc
    //   A unicode-escape-sequence representing a character of the class Pc

    return cat == ConnectorPunctuation;
}

/// <summary>
/// Returns true if the Unicode character is a formatting character (Unicode class Cf).
/// </summary>
bool IsFormattingChar(char16_t ch)
{
    // There are no FormattingChars in ASCII range

    return ch > 127 && IsFormattingChar(GetUnicodeCategory(ch));
}

/// <summary>
/// Returns true if the Unicode character is a formatting character (Unicode class Cf).
/// </summary>
bool IsFormattingChar(int cat)
{
    // formatting-character:
    //   A Unicode character of the class Cf
    //   A unicode-escape-sequence representing a character of the class Cf

    return cat == Format;
}

// ---- Syntax/SyntaxFacts.cs (the green-token extension methods the parser uses) ------------------------------

bool IsIdentifierVar(const GreenNode *node)
{
    return node->contextual_kind == SyntaxKind::VarKeyword;
}

bool IsIdentifierVarOrPredefinedType(const GreenNode *node)
{
    return IsIdentifierVar(node) || IsPredefinedType(node->kind);
}

}  // namespace SyntaxFacts

// ---- .NET's char.GetUnicodeCategory ----------------------------------------------------------------------------

namespace {
struct UnicodeRange {
    char16_t first;
    char16_t last;
    uint8_t category;
};
const UnicodeRange kUnicodeRanges[] = {
#include "csharp_unicode.inc"
};
}  // namespace

int GetUnicodeCategory(char16_t ch)
{
    // The ranges are sorted and cover every UTF-16 unit: find the last one starting at or before ch.
    const UnicodeRange *end = kUnicodeRanges + sizeof(kUnicodeRanges) / sizeof(kUnicodeRanges[0]);
    const UnicodeRange *it =
        std::upper_bound(kUnicodeRanges, end, ch, [](char16_t c, const UnicodeRange &r) { return c < r.first; });
    return it[-1].category;
}

}  // namespace csharp
