/****************************************************************************
**
** Copyright (C) 2015 The Qt Company Ltd.
** Contact: http://www.qt.io/licensing
**
** This file is part of Qt Creator.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Digia.  For licensing terms and
** conditions see http://www.qt.io/licensing.  For further information
** use the contact form at http://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 or version 3 as published by the Free
** Software Foundation and appearing in the file LICENSE.LGPLv21 and
** LICENSE.LGPLv3 included in the packaging of this file.  Please review the
** following information to ensure the GNU Lesser General Public License
** requirements will be met: https://www.gnu.org/licenses/lgpl.html and
** http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Digia gives you certain additional
** rights.  These rights are described in the Digia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
****************************************************************************/

#include "codecompletionchunkconverter.h"

#include "clangstring.h"

namespace ClangBackEnd {

void CodeCompletionChunkConverter::extractCompletionChunks(CXCompletionString completionString)
{
    const uint completionChunkCount = clang_getNumCompletionChunks(completionString);
    chunks.reserve(completionChunkCount);

    for (uint chunkIndex = 0; chunkIndex < completionChunkCount; ++chunkIndex) {
        const CodeCompletionChunk::Kind kind = chunkKind(completionString, chunkIndex);

        if (kind == CodeCompletionChunk::Optional)
            chunks.append(CodeCompletionChunk(kind,
                                              chunkText(completionString, chunkIndex),
                                              optionalChunks(completionString, chunkIndex)));
        else
            chunks.append(CodeCompletionChunk(kind,
                                              chunkText(completionString, chunkIndex)));
    }
}

void CodeCompletionChunkConverter::extractOptionalCompletionChunks(CXCompletionString completionString)
{
    const uint completionChunkCount = clang_getNumCompletionChunks(completionString);

    for (uint chunkIndex = 0; chunkIndex < completionChunkCount; ++chunkIndex) {
        const CodeCompletionChunk::Kind kind = chunkKind(completionString, chunkIndex);

        if (kind == CodeCompletionChunk::Optional)
            extractOptionalCompletionChunks(clang_getCompletionChunkCompletionString(completionString, chunkIndex));
        else
            chunks.append(CodeCompletionChunk(kind, chunkText(completionString, chunkIndex)));
    }
}

CodeCompletionChunk::Kind CodeCompletionChunkConverter::chunkKind(CXCompletionString completionString, uint chunkIndex)
{
    return CodeCompletionChunk::Kind(clang_getCompletionChunkKind(completionString, chunkIndex));
}

QVector<CodeCompletionChunk> CodeCompletionChunkConverter::extract(CXCompletionString completionString)
{
    CodeCompletionChunkConverter converter;

    converter.extractCompletionChunks(completionString);

    return converter.chunks;
}

Utf8String CodeCompletionChunkConverter::chunkText(CXCompletionString completionString, uint chunkIndex)
{
    return ClangString(clang_getCompletionChunkText(completionString, chunkIndex));
}

QVector<CodeCompletionChunk> CodeCompletionChunkConverter::optionalChunks(CXCompletionString completionString, uint chunkIndex)
{
    CodeCompletionChunkConverter converter;

    converter.extractOptionalCompletionChunks(clang_getCompletionChunkCompletionString(completionString, chunkIndex));

    return converter.chunks;
}

} // namespace ClangBackEnd

