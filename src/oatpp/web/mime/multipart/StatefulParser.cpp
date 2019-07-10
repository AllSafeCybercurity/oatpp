/***************************************************************************
 *
 * Project         _____    __   ____   _      _
 *                (  _  )  /__\ (_  _)_| |_  _| |_
 *                 )(_)(  /(__)\  )( (_   _)(_   _)
 *                (_____)(__)(__)(__)  |_|    |_|
 *
 *
 * Copyright 2018-present, Leonid Stryzhevskyi <lganzzzo@gmail.com>
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
 *
 ***************************************************************************/

#include "StatefulParser.hpp"

#include "oatpp/web/protocol/http/Http.hpp"

#include "oatpp/core/parser/Caret.hpp"


namespace oatpp { namespace web { namespace mime { namespace multipart {

void StatefulParser::onPartHeaders(const Headers& partHeaders) {

  m_currPartIndex ++;

  auto it = partHeaders.find("Content-Disposition");
  if(it != partHeaders.end()) {

    parser::Caret caret(it->second.toString());

    if(caret.findText((p_char8)"name=", 5)) {
      caret.inc(5);

      parser::Caret::Label nameLabel(nullptr);

      if(caret.isAtChar('"')) {
        nameLabel = caret.parseStringEnclosed('"', '"', '\\');
      } else if(caret.isAtChar('\'')) {
        nameLabel = caret.parseStringEnclosed('\'', '\'', '\\');
      } else {
        nameLabel = caret.putLabel();
        caret.findCharFromSet(" \t\n\r\f");
        nameLabel.end();
      }

      if(nameLabel) {

        m_currPartName = nameLabel.toString();

        OATPP_LOGD("Part", "name='%s'", m_currPartName->getData());

        for(auto& pair : partHeaders) {
          auto key = pair.first.toString();
          auto value = pair.second.toString();
          OATPP_LOGD("header", "key='%s', value='%s'", key->getData(), value->getData());
        }

      } else {
        throw std::runtime_error("[oatpp::web::mime::multipart::StatefulParser::onPartHeaders()]: Error. Can't parse part name.");
      }

    } else {
      throw std::runtime_error("[oatpp::web::mime::multipart::StatefulParser::onPartHeaders()]: Error. Part name is missing.");
    }

  } else {
    throw std::runtime_error("[oatpp::web::mime::multipart::StatefulParser::onPartHeaders()]: Error. Missing 'Content-Disposition' header.");
  }

}

void StatefulParser::onPartData(p_char8 data, v_int32 size) {

  oatpp::String text((const char*)data, size, true);
  OATPP_LOGD("data", "part='%s', data='%s'", m_currPartName->getData(), text->getData());

}

v_int32 StatefulParser::parseNext_Boundary(p_char8 data, v_int32 size) {

  p_char8 sampleData = m_nextBoundarySample->getData();
  v_int32 sampleSize = m_nextBoundarySample->getSize();

  if (m_currPartIndex == 0) {
    sampleData = m_firstBoundarySample->getData();
    sampleSize = m_firstBoundarySample->getSize();
  } else {
    sampleData = m_nextBoundarySample->getData();
    sampleSize = m_nextBoundarySample->getSize();
  }

  v_int32 checkSize = sampleSize - m_currBoundaryCharIndex;
  if(checkSize > size) {
    checkSize = size;
  }

  parser::Caret caret(data, size);

  if(caret.isAtText(&sampleData[m_currBoundaryCharIndex], checkSize, true)) {

    m_currBoundaryCharIndex += caret.getPosition();

    if(m_currBoundaryCharIndex == sampleSize) {
      m_state = STATE_AFTER_BOUNDARY;
      m_currBoundaryCharIndex = 0;
      m_readingBody = false;
    }

    return caret.getPosition();

  } else if(m_readingBody) {

    if(m_currBoundaryCharIndex > 0) {
      onPartData(sampleData, m_currBoundaryCharIndex);
    }

    m_state = STATE_DATA;
    m_currBoundaryCharIndex = 0;
    m_checkForBoundary = false;

    return 0;

  }

  throw std::runtime_error("[oatpp::web::mime::multipart::StatefulParser::parseNext_Boundary()]: Error. Invalid state.");

}

v_int32 StatefulParser::parseNext_AfterBoundary(p_char8 data, v_int32 size) {

  if(m_currBoundaryCharIndex == 0) {

    if(data[0] == '-') {
      m_finishingBoundary = true;
    } else if(data[0] != '\r') {
      throw std::runtime_error("[oatpp::web::mime::multipart::StatefulParser::parseNext_AfterBoundary()]: Error. Invalid char.");
    }

  }

  if(size > 1 || m_currBoundaryCharIndex == 1) {

    if (m_finishingBoundary && data[1 - m_currBoundaryCharIndex] == '-') {
      m_state = STATE_DONE;
      m_currBoundaryCharIndex = 0;
      return 2 - m_currBoundaryCharIndex;
    } else if (!m_finishingBoundary && data[1 - m_currBoundaryCharIndex] == '\n') {
      m_state = STATE_HEADERS;
      m_currBoundaryCharIndex = 0;
      m_headerSectionEndAccumulator = 0;
      return 2 - m_currBoundaryCharIndex;
    } else {
      throw std::runtime_error("[oatpp::web::mime::multipart::StatefulParser::parseNext_AfterBoundary()]: Error. Invalid trailing char.");
    }

  }

  m_currBoundaryCharIndex = 1;
  return 1;

}

v_int32 StatefulParser::parseNext_Headers(p_char8 data, v_int32 size) {

  for(v_int32 i = 0; i < size; i ++) {

    m_headerSectionEndAccumulator <<= 8;
    m_headerSectionEndAccumulator |= data[i];

    if(m_headerSectionEndAccumulator == HEADERS_SECTION_END) {

      m_headersBuffer.write(data, i);

      auto headersText = m_headersBuffer.toString();
      m_headersBuffer.clear();

      protocol::http::Status status;
      parser::Caret caret(headersText);
      Headers headers;

      protocol::http::Parser::parseHeaders(headers, headersText.getPtr(), caret, status);

      onPartHeaders(headers);

      m_state = STATE_DATA;
      m_checkForBoundary = true;

      return i + 1;

    }

  }

  m_headersBuffer.write(data, size);

  return size;
}

v_int32 StatefulParser::parseNext_Data(p_char8 data, v_int32 size) {

  parser::Caret caret(data, size);

  bool rFound = caret.findChar('\r');
  if(rFound && !m_checkForBoundary) {
    caret.inc();
    rFound = caret.findChar('\r');
  }

  m_checkForBoundary = true;

  if(rFound) {
    if(caret.getPosition() > 0) {
      onPartData(data, caret.getPosition());
    }
    m_state = STATE_BOUNDARY;
    m_readingBody = true;
    return caret.getPosition();
  } else {
    onPartData(data, size);
  }

  return size;
}

v_int32 StatefulParser::parseNext(p_char8 data, v_int32 size) {

  v_int32 pos = 0;

  while(pos < size) {

    switch (m_state) {
      case STATE_BOUNDARY:
        pos += parseNext_Boundary(&data[pos], size - pos);
        break;
      case STATE_AFTER_BOUNDARY:
        pos += parseNext_AfterBoundary(&data[pos], size - pos);
        break;
      case STATE_HEADERS:
        pos += parseNext_Headers(&data[pos], size - pos);
        break;
      case STATE_DATA:
        pos += parseNext_Data(&data[pos], size - pos);
        break;
      case STATE_DONE:
        return pos;
      default:
        throw std::runtime_error("[oatpp::web::mime::multipart::StatefulParser::parseNext()]: Error. Invalid state.");
    }

  }

  return pos;

}

}}}}