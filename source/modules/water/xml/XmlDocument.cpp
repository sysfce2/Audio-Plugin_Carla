/*
  ==============================================================================

   This file is part of the Water library.
   Copyright (c) 2016 ROLI Ltd.
   Copyright (C) 2017-2025 Filipe Coelho <falktx@falktx.com>

   Permission is granted to use this software under the terms of the ISC license
   http://www.isc.org/downloads/software-support-policy/isc-license/

   Permission to use, copy, modify, and/or distribute this software for any
   purpose with or without fee is hereby granted, provided that the above
   copyright notice and this permission notice appear in all copies.

   THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES WITH REGARD
   TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
   FITNESS. IN NO EVENT SHALL ISC BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT,
   OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF
   USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
   TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
   OF THIS SOFTWARE.

  ==============================================================================
*/

#include "XmlDocument.h"
#include "XmlElement.h"
#include "../containers/LinkedListPointer.h"
#include "../streams/FileInputSource.h"
#include "../streams/InputStream.h"
#include "../streams/MemoryOutputStream.h"

namespace water {

XmlDocument::XmlDocument (const String& documentText)
    : originalText (documentText),
      input (nullptr),
      outOfData (false),
      errorOccurred (false),
      needToLoadDTD (false),
      ignoreEmptyTextElements (true)
{
}

XmlDocument::XmlDocument (const File& file)
    : input (nullptr),
      outOfData (false),
      errorOccurred (false),
      needToLoadDTD (false),
      ignoreEmptyTextElements (true),
      inputSource (new FileInputSource (file))
{
}

XmlDocument::~XmlDocument()
{
}

XmlElement* XmlDocument::parse (const File& file)
{
    XmlDocument doc (file);
    return doc.getDocumentElement();
}

XmlElement* XmlDocument::parse (const String& xmlData)
{
    XmlDocument doc (xmlData);
    return doc.getDocumentElement();
}

void XmlDocument::setInputSource (FileInputSource* const newSource) noexcept
{
    inputSource = newSource;
}

void XmlDocument::setEmptyTextElementsIgnored (const bool shouldBeIgnored) noexcept
{
    ignoreEmptyTextElements = shouldBeIgnored;
}

namespace XmlIdentifierChars
{
    static bool isIdentifierCharSlow (const water_uchar c) noexcept
    {
        return CharacterFunctions::isLetterOrDigit (c)
                 || c == '_' || c == '-' || c == ':' || c == '.';
    }

    static bool isIdentifierChar (const water_uchar c) noexcept
    {
        static const uint32 legalChars[] = { 0, 0x7ff6000, 0x87fffffe, 0x7fffffe, 0 };

        return ((int) c < (int) numElementsInArray (legalChars) * 32) ? ((legalChars [c >> 5] & (1 << (c & 31))) != 0)
                                                                      : isIdentifierCharSlow (c);
    }

    /*static void generateIdentifierCharConstants()
    {
        uint32 n[8] = { 0 };
        for (int i = 0; i < 256; ++i)
            if (isIdentifierCharSlow (i))
                n[i >> 5] |= (1 << (i & 31));

        String s;
        for (int i = 0; i < 8; ++i)
            s << "0x" << String::toHexString ((int) n[i]) << ", ";

        DBG (s);
    }*/

    static CharPointer_UTF8 findEndOfToken (CharPointer_UTF8 p)
    {
        while (isIdentifierChar (*p))
            ++p;

        return p;
    }
}

XmlElement* XmlDocument::getDocumentElement (const bool onlyReadOuterDocumentElement)
{
    if (originalText.isEmpty() && inputSource != nullptr)
    {
        ScopedPointer<InputStream> in (inputSource->createInputStream());

        if (in != nullptr)
        {
            MemoryOutputStream data;
            data.writeFromInputStream (*in, onlyReadOuterDocumentElement ? 8192 : -1);

            if (data.getDataSize() > 2)
            {
                data.writeByte (0);
                const char* text = static_cast<const char*> (data.getData());

                if (CharPointer_UTF8::isByteOrderMark (text))
                    text += 3;

                // parse the input buffer directly to avoid copying it all to a string..
                return parseDocumentElement (CharPointer_UTF8 (text), onlyReadOuterDocumentElement);
            }
        }
    }

    return parseDocumentElement (originalText.getCharPointer(), onlyReadOuterDocumentElement);
}

const String& XmlDocument::getLastParseError() const noexcept
{
    return lastError;
}

void XmlDocument::setLastError (const String& desc, const bool carryOn)
{
    lastError = desc;
    errorOccurred = ! carryOn;
}

String XmlDocument::getFileContents (const String& filename) const
{
    if (inputSource != nullptr)
    {
        const ScopedPointer<InputStream> in (inputSource->createInputStreamFor (filename.trim().unquoted()));

        if (in != nullptr)
            return in->readEntireStreamAsString();
    }

    return String();
}

water_uchar XmlDocument::readNextChar() noexcept
{
    const water_uchar c = input.getAndAdvance();

    if (c == 0)
    {
        outOfData = true;
        --input;
    }

    return c;
}

XmlElement* XmlDocument::parseDocumentElement (CharPointer_UTF8 textToParse,
                                               const bool onlyReadOuterDocumentElement)
{
    input = textToParse;
    errorOccurred = false;
    outOfData = false;
    needToLoadDTD = true;

    if (textToParse.isEmpty())
    {
        lastError = "not enough input";
    }
    else if (! parseHeader())
    {
        lastError = "malformed header";
    }
    else if (! parseDTD())
    {
        lastError = "malformed DTD";
    }
    else
    {
        lastError.clear();

        ScopedPointer<XmlElement> result (readNextElement (! onlyReadOuterDocumentElement));

        if (! errorOccurred)
            return result.release();
    }

    return nullptr;
}

bool XmlDocument::parseHeader()
{
    skipNextWhiteSpace();

    if (CharacterFunctions::compareUpTo (input, CharPointer_UTF8 ("<?xml"), 5) == 0)
    {
        const CharPointer_UTF8 headerEnd (CharacterFunctions::find (input, CharPointer_UTF8 ("?>")));

        if (headerEnd.isEmpty())
            return false;

        const String encoding (String (input, headerEnd)
                                 .fromFirstOccurrenceOf ("encoding", false, true)
                                 .fromFirstOccurrenceOf ("=", false, false)
                                 .fromFirstOccurrenceOf ("\"", false, false)
                                 .upToFirstOccurrenceOf ("\"", false, false).trim());

        /* If you load an XML document with a non-UTF encoding type, it may have been
           loaded wrongly.. Since all the files are read via the normal water file streams,
           they're treated as UTF-8, so by the time it gets to the parser, the encoding will
           have been lost. Best plan is to stick to utf-8 or if you have specific files to
           read, use your own code to convert them to a unicode String, and pass that to the
           XML parser.
        */
        CARLA_SAFE_ASSERT_RETURN (encoding.isEmpty() || encoding.startsWithIgnoreCase ("utf-"), false);

        input = headerEnd + 2;
        skipNextWhiteSpace();
    }

    return true;
}

bool XmlDocument::parseDTD()
{
    if (CharacterFunctions::compareUpTo (input, CharPointer_UTF8 ("<!DOCTYPE"), 9) == 0)
    {
        input += 9;
        const CharPointer_UTF8 dtdStart (input);

        for (int n = 1; n > 0;)
        {
            const water_uchar c = readNextChar();

            if (outOfData)
                return false;

            if (c == '<')
                ++n;
            else if (c == '>')
                --n;
        }

        dtdText = String (dtdStart, input - 1).trim();
    }

    return true;
}

void XmlDocument::skipNextWhiteSpace()
{
    for (;;)
    {
        input = input.findEndOfWhitespace();

        if (input.isEmpty())
        {
            outOfData = true;
            break;
        }

        if (*input == '<')
        {
            if (input[1] == '!'
                 && input[2] == '-'
                 && input[3] == '-')
            {
                input += 4;
                const int closeComment = input.indexOf (CharPointer_UTF8 ("-->"));

                if (closeComment < 0)
                {
                    outOfData = true;
                    break;
                }

                input += closeComment + 3;
                continue;
            }

            if (input[1] == '?')
            {
                input += 2;
                const int closeBracket = input.indexOf (CharPointer_UTF8 ("?>"));

                if (closeBracket < 0)
                {
                    outOfData = true;
                    break;
                }

                input += closeBracket + 2;
                continue;
            }
        }

        break;
    }
}

void XmlDocument::readQuotedString (String& result)
{
    const water_uchar quote = readNextChar();

    while (! outOfData)
    {
        const water_uchar c = readNextChar();

        if (c == quote)
            break;

        --input;

        if (c == '&')
        {
            readEntity (result);
        }
        else
        {
            const CharPointer_UTF8 start (input);

            for (;;)
            {
                const water_uchar character = *input;

                if (character == quote)
                {
                    result.appendCharPointer (start, input);
                    ++input;
                    return;
                }
                else if (character == '&')
                {
                    result.appendCharPointer (start, input);
                    break;
                }
                else if (character == 0)
                {
                    setLastError ("unmatched quotes", false);
                    outOfData = true;
                    break;
                }

                ++input;
            }
        }
    }
}

XmlElement* XmlDocument::readNextElement (const bool alsoParseSubElements)
{
    XmlElement* node = nullptr;

    skipNextWhiteSpace();
    if (outOfData)
        return nullptr;

    if (*input == '<')
    {
        ++input;
        CharPointer_UTF8 endOfToken (XmlIdentifierChars::findEndOfToken (input));

        if (endOfToken == input)
        {
            // no tag name - but allow for a gap after the '<' before giving an error
            skipNextWhiteSpace();
            endOfToken = XmlIdentifierChars::findEndOfToken (input);

            if (endOfToken == input)
            {
                setLastError ("tag name missing", false);
                return node;
            }
        }

        node = new XmlElement (input, endOfToken);
        input = endOfToken;
        LinkedListPointer<XmlElement::XmlAttributeNode>::Appender attributeAppender (node->attributes);

        // look for attributes
        for (;;)
        {
            skipNextWhiteSpace();

            const water_uchar c = *input;

            // empty tag..
            if (c == '/' && input[1] == '>')
            {
                input += 2;
                break;
            }

            // parse the guts of the element..
            if (c == '>')
            {
                ++input;

                if (alsoParseSubElements)
                    readChildElements (*node);

                break;
            }

            // get an attribute..
            if (XmlIdentifierChars::isIdentifierChar (c))
            {
                CharPointer_UTF8 attNameEnd (XmlIdentifierChars::findEndOfToken (input));

                if (attNameEnd != input)
                {
                    const CharPointer_UTF8 attNameStart (input);
                    input = attNameEnd;

                    skipNextWhiteSpace();

                    if (readNextChar() == '=')
                    {
                        skipNextWhiteSpace();

                        const water_uchar nextChar = *input;

                        if (nextChar == '"' || nextChar == '\'')
                        {
                            XmlElement::XmlAttributeNode* const newAtt
                                = new XmlElement::XmlAttributeNode (attNameStart, attNameEnd);

                            readQuotedString (newAtt->value);
                            attributeAppender.append (newAtt);
                            continue;
                        }
                    }
                    else
                    {
                        setLastError ("expected '=' after attribute '"
                                        + String (attNameStart, attNameEnd) + "'", false);
                        return node;
                    }
                }
            }
            else
            {
                if (! outOfData)
                    setLastError ("illegal character found in " + node->getTagName() + ": '" + c + "'", false);
            }

            break;
        }
    }

    return node;
}

void XmlDocument::readChildElements (XmlElement& parent)
{
    LinkedListPointer<XmlElement>::Appender childAppender (parent.firstChildElement);

    for (;;)
    {
        const CharPointer_UTF8 preWhitespaceInput (input);
        skipNextWhiteSpace();

        if (outOfData)
        {
            setLastError ("unmatched tags", false);
            break;
        }

        if (*input == '<')
        {
            const water_uchar c1 = input[1];

            if (c1 == '/')
            {
                // our close tag..
                const int closeTag = input.indexOf ((water_uchar) '>');

                if (closeTag >= 0)
                    input += closeTag + 1;

                break;
            }

            if (c1 == '!' && CharacterFunctions::compareUpTo (input + 2, CharPointer_UTF8 ("[CDATA["), 7) == 0)
            {
                input += 9;
                const CharPointer_UTF8 inputStart (input);

                for (;;)
                {
                    const water_uchar c0 = *input;

                    if (c0 == 0)
                    {
                        setLastError ("unterminated CDATA section", false);
                        outOfData = true;
                        break;
                    }
                    else if (c0 == ']'
                              && input[1] == ']'
                              && input[2] == '>')
                    {
                        childAppender.append (XmlElement::createTextElement (String (inputStart, input)));
                        input += 3;
                        break;
                    }

                    ++input;
                }
            }
            else
            {
                // this is some other element, so parse and add it..
                if (XmlElement* const n = readNextElement (true))
                    childAppender.append (n);
                else
                    break;
            }
        }
        else  // must be a character block
        {
            input = preWhitespaceInput; // roll back to include the leading whitespace
            MemoryOutputStream textElementContent;
            bool contentShouldBeUsed = ! ignoreEmptyTextElements;

            for (;;)
            {
                const water_uchar c = *input;

                if (c == '<')
                {
                    if (input[1] == '!' && input[2] == '-' && input[3] == '-')
                    {
                        input += 4;
                        const int closeComment = input.indexOf (CharPointer_UTF8 ("-->"));

                        if (closeComment < 0)
                        {
                            setLastError ("unterminated comment", false);
                            outOfData = true;
                            return;
                        }

                        input += closeComment + 3;
                        continue;
                    }

                    break;
                }

                if (c == 0)
                {
                    setLastError ("unmatched tags", false);
                    outOfData = true;
                    return;
                }

                if (c == '&')
                {
                    String entity;
                    readEntity (entity);

                    if (entity.startsWithChar ('<') && entity [1] != 0)
                    {
                        const CharPointer_UTF8 oldInput (input);
                        const bool oldOutOfData = outOfData;

                        input = entity.getCharPointer();
                        outOfData = false;

                        while (XmlElement* n = readNextElement (true))
                            childAppender.append (n);

                        input = oldInput;
                        outOfData = oldOutOfData;
                    }
                    else
                    {
                        textElementContent << entity;
                        contentShouldBeUsed = contentShouldBeUsed || entity.containsNonWhitespaceChars();
                    }
                }
                else
                {
                    for (;; ++input)
                    {
                        water_uchar nextChar = *input;

                        if (nextChar == '\r')
                        {
                            nextChar = '\n';

                            if (input[1] == '\n')
                                continue;
                        }

                        if (nextChar == '<' || nextChar == '&')
                            break;

                        if (nextChar == 0)
                        {
                            setLastError ("unmatched tags", false);
                            outOfData = true;
                            return;
                        }

                        textElementContent.appendUTF8Char (nextChar);
                        contentShouldBeUsed = contentShouldBeUsed || ! CharacterFunctions::isWhitespace (nextChar);
                    }
                }
            }

            if (contentShouldBeUsed)
                childAppender.append (XmlElement::createTextElement (textElementContent.toUTF8()));
        }
    }
}

void XmlDocument::readEntity (String& result)
{
    // skip over the ampersand
    ++input;

    if (input.compareIgnoreCaseUpTo (CharPointer_UTF8 ("amp;"), 4) == 0)
    {
        input += 4;
        result += '&';
    }
    else if (input.compareIgnoreCaseUpTo (CharPointer_UTF8 ("quot;"), 5) == 0)
    {
        input += 5;
        result += '"';
    }
    else if (input.compareIgnoreCaseUpTo (CharPointer_UTF8 ("apos;"), 5) == 0)
    {
        input += 5;
        result += '\'';
    }
    else if (input.compareIgnoreCaseUpTo (CharPointer_UTF8 ("lt;"), 3) == 0)
    {
        input += 3;
        result += '<';
    }
    else if (input.compareIgnoreCaseUpTo (CharPointer_UTF8 ("gt;"), 3) == 0)
    {
        input += 3;
        result += '>';
    }
    else if (*input == '#')
    {
        int charCode = 0;
        ++input;

        if (*input == 'x' || *input == 'X')
        {
            ++input;
            int numChars = 0;

            while (input[0] != ';')
            {
                const int hexValue = CharacterFunctions::getHexDigitValue (input[0]);

                if (hexValue < 0 || ++numChars > 8)
                {
                    setLastError ("illegal escape sequence", true);
                    break;
                }

                charCode = (charCode << 4) | hexValue;
                ++input;
            }

            ++input;
        }
        else if (input[0] >= '0' && input[0] <= '9')
        {
            int numChars = 0;

            while (input[0] != ';')
            {
                if (++numChars > 12)
                {
                    setLastError ("illegal escape sequence", true);
                    break;
                }

                charCode = charCode * 10 + ((int) input[0] - '0');
                ++input;
            }

            ++input;
        }
        else
        {
            setLastError ("illegal escape sequence", true);
            result += '&';
            return;
        }

        result << (water_uchar) charCode;
    }
    else
    {
        const CharPointer_UTF8 entityNameStart (input);
        const int closingSemiColon = input.indexOf ((water_uchar) ';');

        if (closingSemiColon < 0)
        {
            outOfData = true;
            result += '&';
        }
        else
        {
            input += closingSemiColon + 1;

            result += expandExternalEntity (String (entityNameStart, (size_t) closingSemiColon));
        }
    }
}

String XmlDocument::expandEntity (const String& ent)
{
    if (ent.equalsIgnoreCase ("amp"))   return String::charToString ('&');
    if (ent.equalsIgnoreCase ("quot"))  return String::charToString ('"');
    if (ent.equalsIgnoreCase ("apos"))  return String::charToString ('\'');
    if (ent.equalsIgnoreCase ("lt"))    return String::charToString ('<');
    if (ent.equalsIgnoreCase ("gt"))    return String::charToString ('>');

    if (ent[0] == '#')
    {
        const water_uchar char1 = ent[1];

        if (char1 == 'x' || char1 == 'X')
            return String::charToString (static_cast<water_uchar> (ent.substring (2).getHexValue32()));

        if (char1 >= '0' && char1 <= '9')
            return String::charToString (static_cast<water_uchar> (ent.substring (1).getIntValue()));

        setLastError ("illegal escape sequence", false);
        return String::charToString ('&');
    }

    return expandExternalEntity (ent);
}

String XmlDocument::expandExternalEntity (const String& entity)
{
    if (needToLoadDTD)
    {
        if (dtdText.isNotEmpty())
        {
            dtdText = dtdText.trimCharactersAtEnd (">");
            tokenisedDTD.addTokens (dtdText, true);

            if (tokenisedDTD [tokenisedDTD.size() - 2].equalsIgnoreCase ("system")
                 && tokenisedDTD [tokenisedDTD.size() - 1].isQuotedString())
            {
                const String fn (tokenisedDTD [tokenisedDTD.size() - 1]);

                tokenisedDTD.clear();
                tokenisedDTD.addTokens (getFileContents (fn), true);
            }
            else
            {
                tokenisedDTD.clear();
                const int openBracket = dtdText.indexOfChar ('[');

                if (openBracket > 0)
                {
                    const int closeBracket = dtdText.lastIndexOfChar (']');

                    if (closeBracket > openBracket)
                        tokenisedDTD.addTokens (dtdText.substring (openBracket + 1,
                                                                   closeBracket), true);
                }
            }

            for (int i = tokenisedDTD.size(); --i >= 0;)
            {
                if (tokenisedDTD[i].startsWithChar ('%')
                     && tokenisedDTD[i].endsWithChar (';'))
                {
                    const String parsed (getParameterEntity (tokenisedDTD[i].substring (1, tokenisedDTD[i].length() - 1)));
                    StringArray newToks;
                    newToks.addTokens (parsed, true);

                    tokenisedDTD.remove (i);

                    for (int j = newToks.size(); --j >= 0;)
                        tokenisedDTD.insert (i, newToks[j]);
                }
            }
        }

        needToLoadDTD = false;
    }

    for (int i = 0; i < tokenisedDTD.size(); ++i)
    {
        if (tokenisedDTD[i] == entity)
        {
            if (tokenisedDTD[i - 1].equalsIgnoreCase ("<!entity"))
            {
                String ent (tokenisedDTD [i + 1].trimCharactersAtEnd (">").trim().unquoted());

                // check for sub-entities..
                int ampersand = ent.indexOfChar ('&');

                while (ampersand >= 0)
                {
                    const int semiColon = ent.indexOf (i + 1, ";");

                    if (semiColon < 0)
                    {
                        setLastError ("entity without terminating semi-colon", false);
                        break;
                    }

                    const String resolved (expandEntity (ent.substring (i + 1, semiColon)));

                    ent = ent.substring (0, ampersand)
                           + resolved
                           + ent.substring (semiColon + 1);

                    ampersand = ent.indexOfChar (semiColon + 1, '&');
                }

                return ent;
            }
        }
    }

    setLastError ("unknown entity", true);

    return entity;
}

String XmlDocument::getParameterEntity (const String& entity)
{
    for (int i = 0; i < tokenisedDTD.size(); ++i)
    {
        if (tokenisedDTD[i] == entity
             && tokenisedDTD [i - 1] == "%"
             && tokenisedDTD [i - 2].equalsIgnoreCase ("<!entity"))
        {
            const String ent (tokenisedDTD [i + 1].trimCharactersAtEnd (">"));

            if (ent.equalsIgnoreCase ("system"))
                return getFileContents (tokenisedDTD [i + 2].trimCharactersAtEnd (">"));

            return ent.trim().unquoted();
        }
    }

    return entity;
}

}
