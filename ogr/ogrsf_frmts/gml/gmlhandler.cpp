/**********************************************************************
 *
 * Project:  GML Reader
 * Purpose:  Implementation of GMLHandler class.
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 *
 **********************************************************************
 * Copyright (c) 2002, Frank Warmerdam
 * Copyright (c) 2008-2014, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "cpl_port.h"
#include "gmlreader.h"
#include "gmlreaderp.h"

#include <algorithm>
#include <climits>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "cpl_conv.h"
#include "cpl_error.h"
#include "cpl_hash_set.h"
#include "cpl_minixml.h"
#include "cpl_string.h"
#include "cpl_vsi.h"
#ifdef HAVE_EXPAT
#include "expat.h"
#include "expat_external.h"
#endif
#include "ogr_core.h"
#ifdef HAVE_XERCES
#include "ogr_xerces.h"
#endif

#ifdef HAVE_XERCES

/************************************************************************/
/*                        GMLXercesHandler()                            */
/************************************************************************/

GMLXercesHandler::GMLXercesHandler(GMLReader *poReader)
    : GMLHandler(poReader), m_nEntityCounter(0)
{
}

/************************************************************************/
/*                    GMLXercesHandlerDealWithError()                   */
/************************************************************************/

static void GMLXercesHandlerDealWithError(OGRErr eErr)
{
    if (eErr == OGRERR_NOT_ENOUGH_MEMORY)
    {
        throw SAXNotSupportedException("Out of memory");
    }
    else if (eErr != OGRERR_NONE)
    {
        throw SAXNotSupportedException("Other error during parsing");
    }
}

/************************************************************************/
/*                            startElement()                            */
/************************************************************************/

void GMLXercesHandler::startElement(const XMLCh *const /*uri*/,
                                    const XMLCh *const localname,
                                    const XMLCh *const /*qname*/,
                                    const Attributes &attrs)
{
    m_nEntityCounter = 0;

    transcode(localname, m_osElement);

    GMLXercesHandlerDealWithError(GMLHandler::startElement(
        m_osElement.c_str(), static_cast<int>(m_osElement.size()),
        const_cast<Attributes *>(&attrs)));
}

/************************************************************************/
/*                             endElement()                             */
/************************************************************************/
void GMLXercesHandler::endElement(const XMLCh *const /*uri*/,
                                  const XMLCh *const /*localname*/,
                                  const XMLCh *const /*qname */)
{
    m_nEntityCounter = 0;

    GMLXercesHandlerDealWithError(GMLHandler::endElement());
}

/************************************************************************/
/*                             characters()                             */
/************************************************************************/

void GMLXercesHandler::characters(const XMLCh *const chars_in,
                                  const XMLSize_t length)

{
    transcode(chars_in, m_osCharacters, static_cast<int>(length));
    GMLXercesHandlerDealWithError(GMLHandler::dataHandler(
        m_osCharacters.c_str(), static_cast<int>(m_osCharacters.size())));
}

/************************************************************************/
/*                             fatalError()                             */
/************************************************************************/

void GMLXercesHandler::fatalError(const SAXParseException &exception)

{
    CPLString osMsg;
    transcode(exception.getMessage(), osMsg);
    CPLError(CE_Failure, CPLE_AppDefined,
             "XML Parsing Error: %s at line %d, column %d\n", osMsg.c_str(),
             static_cast<int>(exception.getLineNumber()),
             static_cast<int>(exception.getColumnNumber()));
}

/************************************************************************/
/*                             startEntity()                            */
/************************************************************************/

void GMLXercesHandler::startEntity(const XMLCh *const /* name */)
{
    m_nEntityCounter++;
    if (m_nEntityCounter > 1000 && !m_poReader->HasStoppedParsing())
    {
        throw SAXNotSupportedException(
            "File probably corrupted (million laugh pattern)");
    }
}

/************************************************************************/
/*                               GetFID()                               */
/************************************************************************/

const char *GMLXercesHandler::GetFID(void *attr)
{
    const Attributes *attrs = static_cast<const Attributes *>(attr);
    const XMLCh achFID[] = {'f', 'i', 'd', '\0'};
    int nFIDIndex = attrs->getIndex(achFID);
    if (nFIDIndex != -1)
    {
        transcode(attrs->getValue(nFIDIndex), m_osFID);
        return m_osFID.c_str();
    }
    else
    {
        const XMLCh achGMLID[] = {'g', 'm', 'l', ':', 'i', 'd', '\0'};
        nFIDIndex = attrs->getIndex(achGMLID);
        if (nFIDIndex != -1)
        {
            transcode(attrs->getValue(nFIDIndex), m_osFID);
            return m_osFID.c_str();
        }
    }

    m_osFID.resize(0);
    return nullptr;
}

/************************************************************************/
/*                        AddAttributes()                               */
/************************************************************************/

CPLXMLNode *GMLXercesHandler::AddAttributes(CPLXMLNode *psNode, void *attr)
{
    const Attributes *attrs = static_cast<const Attributes *>(attr);

    CPLXMLNode *psLastChild = nullptr;

    for (unsigned int i = 0; i < attrs->getLength(); i++)
    {
        transcode(attrs->getQName(i), m_osAttrName);
        transcode(attrs->getValue(i), m_osAttrValue);

        CPLXMLNode *psChild =
            CPLCreateXMLNode(nullptr, CXT_Attribute, m_osAttrName.c_str());
        CPLCreateXMLNode(psChild, CXT_Text, m_osAttrValue.c_str());

        if (psLastChild == nullptr)
            psNode->psChild = psChild;
        else
            psLastChild->psNext = psChild;
        psLastChild = psChild;
    }

    return psLastChild;
}

/************************************************************************/
/*                    GetAttributeValue()                               */
/************************************************************************/

char *GMLXercesHandler::GetAttributeValue(void *attr,
                                          const char *pszAttributeName)
{
    const Attributes *attrs = static_cast<const Attributes *>(attr);
    for (unsigned int i = 0; i < attrs->getLength(); i++)
    {
        transcode(attrs->getQName(i), m_osAttrName);
        if (m_osAttrName == pszAttributeName)
        {
            transcode(attrs->getValue(i), m_osAttrValue);
            return CPLStrdup(m_osAttrValue);
        }
    }
    return nullptr;
}

/************************************************************************/
/*                    GetAttributeByIdx()                               */
/************************************************************************/

char *GMLXercesHandler::GetAttributeByIdx(void *attr, unsigned int idx,
                                          char **ppszKey)
{
    const Attributes *attrs = static_cast<const Attributes *>(attr);
    if (idx >= attrs->getLength())
    {
        *ppszKey = nullptr;
        return nullptr;
    }
    transcode(attrs->getQName(idx), m_osAttrName);
    transcode(attrs->getValue(idx), m_osAttrValue);

    *ppszKey = CPLStrdup(m_osAttrName);
    return CPLStrdup(m_osAttrValue);
}

#endif

#ifdef HAVE_EXPAT

/************************************************************************/
/*                            GMLExpatHandler()                         */
/************************************************************************/

GMLExpatHandler::GMLExpatHandler(GMLReader *poReader, XML_Parser oParser)
    : GMLHandler(poReader), m_oParser(oParser), m_bStopParsing(false),
      m_nDataHandlerCounter(0)
{
}

/************************************************************************/
/*                  GMLExpatHandler::DealWithError()                    */
/************************************************************************/

void GMLExpatHandler::DealWithError(OGRErr eErr)
{
    if (eErr != OGRERR_NONE)
    {
        m_bStopParsing = true;
        XML_StopParser(m_oParser, static_cast<XML_Bool>(false));
        if (eErr == OGRERR_NOT_ENOUGH_MEMORY)
            CPLError(CE_Failure, CPLE_OutOfMemory, "Out of memory");
    }
}

/************************************************************************/
/*                           startElementCbk()                          */
/************************************************************************/

void XMLCALL GMLExpatHandler::startElementCbk(void *pUserData,
                                              const char *pszName,
                                              const char **ppszAttr)

{
    GMLExpatHandler *pThis = static_cast<GMLExpatHandler *>(pUserData);
    if (pThis->m_bStopParsing)
        return;

    const char *pszIter = pszName;
    char ch = '\0';
    while ((ch = *pszIter) != '\0')
    {
        if (ch == ':')
            pszName = pszIter + 1;
        pszIter++;
    }

    pThis->DealWithError(pThis->GMLHandler::startElement(
        pszName, static_cast<int>(pszIter - pszName), ppszAttr));
}

/************************************************************************/
/*                            endElementCbk()                           */
/************************************************************************/
void XMLCALL GMLExpatHandler::endElementCbk(void *pUserData,
                                            const char * /* pszName */)
{
    GMLExpatHandler *pThis = static_cast<GMLExpatHandler *>(pUserData);
    if (pThis->m_bStopParsing)
        return;

    pThis->DealWithError(pThis->GMLHandler::endElement());
}

/************************************************************************/
/*                            dataHandlerCbk()                          */
/************************************************************************/

void XMLCALL GMLExpatHandler::dataHandlerCbk(void *pUserData, const char *data,
                                             int nLen)

{
    GMLExpatHandler *pThis = static_cast<GMLExpatHandler *>(pUserData);
    if (pThis->m_bStopParsing)
        return;

    pThis->m_nDataHandlerCounter++;
    // The size of the buffer that is fetched and that Expat parses is
    // PARSER_BUF_SIZE bytes. If the dataHandlerCbk() callback is called
    // more than PARSER_BUF_SIZE times, this means that one byte in the
    // file expands to more XML text fragments, which is the sign of a
    // likely abuse of <!ENTITY>
    // Note: the counter is zeroed by ResetDataHandlerCounter() before each
    // new XML parsing.
    if (pThis->m_nDataHandlerCounter >= PARSER_BUF_SIZE)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "File probably corrupted (million laugh pattern)");
        pThis->m_bStopParsing = true;
        XML_StopParser(pThis->m_oParser, static_cast<XML_Bool>(false));
        return;
    }

    pThis->DealWithError(pThis->GMLHandler::dataHandler(data, nLen));
}

/************************************************************************/
/*                               GetFID()                               */
/************************************************************************/

const char *GMLExpatHandler::GetFID(void *attr)
{
    const char *const *papszIter = static_cast<const char *const *>(attr);
    while (*papszIter)
    {
        if (strcmp(*papszIter, "fid") == 0 || strcmp(*papszIter, "gml:id") == 0)
        {
            return papszIter[1];
        }
        papszIter += 2;
    }
    return nullptr;
}

/************************************************************************/
/*                        AddAttributes()                               */
/************************************************************************/

CPLXMLNode *GMLExpatHandler::AddAttributes(CPLXMLNode *psNode, void *attr)
{
    const char **papszIter = static_cast<const char **>(attr);

    CPLXMLNode *psLastChild = nullptr;

    while (*papszIter)
    {
        CPLXMLNode *psChild =
            CPLCreateXMLNode(nullptr, CXT_Attribute, papszIter[0]);
        CPLCreateXMLNode(psChild, CXT_Text, papszIter[1]);

        if (psLastChild == nullptr)
            psNode->psChild = psChild;
        else
            psLastChild->psNext = psChild;
        psLastChild = psChild;

        papszIter += 2;
    }

    return psLastChild;
}

/************************************************************************/
/*                    GetAttributeValue()                               */
/************************************************************************/

char *GMLExpatHandler::GetAttributeValue(void *attr,
                                         const char *pszAttributeName)
{
    const char *const *papszIter = static_cast<const char *const *>(attr);
    while (*papszIter)
    {
        if (strcmp(*papszIter, pszAttributeName) == 0)
        {
            return CPLStrdup(papszIter[1]);
        }
        papszIter += 2;
    }
    return nullptr;
}

/************************************************************************/
/*                    GetAttributeByIdx()                               */
/************************************************************************/

// CAUTION: should be called with increasing idx starting from 0 and
// no attempt to read beyond end of list.
char *GMLExpatHandler::GetAttributeByIdx(void *attr, unsigned int idx,
                                         char **ppszKey)
{
    const char *const *papszIter = static_cast<const char *const *>(attr);
    if (papszIter[2 * idx] == nullptr)
    {
        *ppszKey = nullptr;
        return nullptr;
    }
    *ppszKey = CPLStrdup(papszIter[2 * idx]);
    return CPLStrdup(papszIter[2 * idx + 1]);
}

#endif

static const char *const apszGMLGeometryElements[] = {
    "BoundingBox", /* ows:BoundingBox */
    "CompositeCurve",
    "CompositeSurface",
    "Shell", /* CityGML 3 */
    "Curve",
    "GeometryCollection", /* OGR < 1.8.0 bug... */
    "LineString",
    "MultiCurve",
    "MultiGeometry",
    "MultiLineString",
    "MultiPoint",
    "MultiPolygon",
    "MultiSurface",
    "Point",
    "Polygon",
    "PolygonPatch",
    "PolyhedralSurface",
    "SimplePolygon",    /* GML 3.3 compact encoding */
    "SimpleRectangle",  /* GML 3.3 compact encoding */
    "SimpleTriangle",   /* GML 3.3 compact encoding */
    "SimpleMultiPoint", /* GML 3.3 compact encoding */
    "Solid",
    "Surface",
    "Tin",
    "TopoCurve",
    "TopoSurface",
    "Triangle",
    "TriangulatedSurface"};

#define GML_GEOMETRY_TYPE_COUNT                                                \
    static_cast<int>(sizeof(apszGMLGeometryElements) /                         \
                     sizeof(apszGMLGeometryElements[0]))

bool OGRGMLIsGeometryElement(const char *pszElement)
{
    for (const auto &pszGMLElement : apszGMLGeometryElements)
    {
        if (strcmp(pszElement, pszGMLElement) == 0)
            return true;
    }
    return false;
}

struct _GeometryNamesStruct
{
    unsigned long nHash;
    const char *pszName;
};

/************************************************************************/
/*                            GMLHandler()                              */
/************************************************************************/

GMLHandler::GMLHandler(GMLReader *poReader)
    : pasGeometryNames(static_cast<GeometryNamesStruct *>(
          CPLMalloc(GML_GEOMETRY_TYPE_COUNT * sizeof(GeometryNamesStruct)))),
      m_nSRSDimensionIfMissing(
          atoi(CPLGetConfigOption("GML_SRS_DIMENSION_IF_MISSING", "0"))),
      m_poReader(poReader), eAppSchemaType(APPSCHEMA_GENERIC), nStackDepth(0)
{
    for (int i = 0; i < GML_GEOMETRY_TYPE_COUNT; i++)
    {
        pasGeometryNames[i].pszName = apszGMLGeometryElements[i];
        pasGeometryNames[i].nHash =
            CPLHashSetHashStr(pasGeometryNames[i].pszName);
    }
    std::sort(pasGeometryNames, pasGeometryNames + GML_GEOMETRY_TYPE_COUNT,
              [](const GeometryNamesStruct &a, const GeometryNamesStruct &b)
              { return a.nHash < b.nHash; });

    stateStack[0] = STATE_TOP;
}

/************************************************************************/
/*                            ~GMLHandler()                             */
/************************************************************************/

GMLHandler::~GMLHandler()

{
    if (apsXMLNode.size() >= 2 && apsXMLNode[1].psNode != nullptr)
        CPLDestroyXMLNode(apsXMLNode[1].psNode);

    CPLFree(m_pszCurField);
    CPLFree(m_pszGeometry);
    CPLFree(m_pszCityGMLGenericAttrName);
    CPLFree(m_pszHref);
    CPLFree(m_pszUom);
    CPLFree(m_pszValue);
    CPLFree(m_pszKieli);
    CPLFree(pasGeometryNames);
}

/************************************************************************/
/*                             startElement()                           */
/************************************************************************/

OGRErr GMLHandler::startElement(const char *pszName, int nLenName, void *attr)
{
    OGRErr eRet;
    switch (stateStack[nStackDepth])
    {
        case STATE_TOP:
            eRet = startElementTop(pszName, nLenName, attr);
            break;
        case STATE_DEFAULT:
            eRet = startElementDefault(pszName, nLenName, attr);
            break;
        case STATE_FEATURE:
            eRet = startElementFeatureAttribute(pszName, nLenName, attr);
            break;
        case STATE_PROPERTY:
            eRet = startElementFeatureAttribute(pszName, nLenName, attr);
            break;
        case STATE_FEATUREPROPERTY:
            eRet = startElementFeatureProperty(pszName, nLenName, attr);
            break;
        case STATE_GEOMETRY:
            eRet = startElementGeometry(pszName, nLenName, attr);
            break;
        case STATE_IGNORED_FEATURE:
            eRet = OGRERR_NONE;
            break;
        case STATE_BOUNDED_BY:
            eRet = startElementBoundedBy(pszName, nLenName, attr);
            break;
        case STATE_BOUNDED_BY_IN_FEATURE:
            eRet = startElementGeometry(pszName, nLenName, attr);
            break;
        case STATE_CITYGML_ATTRIBUTE:
            eRet = startElementCityGMLGenericAttr(pszName, nLenName, attr);
            break;
        default:
            eRet = OGRERR_NONE;
            break;
    }
    m_nDepth++;
    if (m_nDepth == 64)
    {
        // Avoid performance issues on files like
        // https://bugs.chromium.org/p/oss-fuzz/issues/detail?id=21737
        if (m_nUnlimitedDepth < 0)
        {
            m_nUnlimitedDepth = EQUAL(
                CPLGetConfigOption("OGR_GML_NESTING_LEVEL", ""), "UNLIMITED");
        }
        if (!m_nUnlimitedDepth)
        {
            CPLError(CE_Failure, CPLE_NotSupported,
                     "Too deep XML nesting level (%d). "
                     "Set the OGR_GML_NESTING_LEVEL configuration option to "
                     "UNLIMITED to remove that limitation.",
                     m_nDepth);
            eRet = OGRERR_FAILURE;
        }
    }
    return eRet;
}

/************************************************************************/
/*                              endElement()                            */
/************************************************************************/

OGRErr GMLHandler::endElement()
{
    m_nDepth--;
    switch (stateStack[nStackDepth])
    {
        case STATE_TOP:
            return OGRERR_NONE;
            break;
        case STATE_DEFAULT:
            return endElementDefault();
            break;
        case STATE_FEATURE:
            return endElementFeature();
            break;
        case STATE_PROPERTY:
            return endElementAttribute();
            break;
        case STATE_FEATUREPROPERTY:
            return endElementFeatureProperty();
            break;
        case STATE_GEOMETRY:
            return endElementGeometry();
            break;
        case STATE_IGNORED_FEATURE:
            return endElementIgnoredFeature();
            break;
        case STATE_BOUNDED_BY:
            return endElementBoundedBy();
            break;
        case STATE_BOUNDED_BY_IN_FEATURE:
            return endElementBoundedByInFeature();
            break;
        case STATE_CITYGML_ATTRIBUTE:
            return endElementCityGMLGenericAttr();
            break;
        default:
            return OGRERR_NONE;
            break;
    }
}

/************************************************************************/
/*                              dataHandler()                           */
/************************************************************************/

OGRErr GMLHandler::dataHandler(const char *data, int nLen)
{
    switch (stateStack[nStackDepth])
    {
        case STATE_TOP:
            return OGRERR_NONE;
            break;
        case STATE_DEFAULT:
            return OGRERR_NONE;
            break;
        case STATE_FEATURE:
            return OGRERR_NONE;
            break;
        case STATE_PROPERTY:
            return dataHandlerAttribute(data, nLen);
            break;
        case STATE_FEATUREPROPERTY:
            return OGRERR_NONE;
            break;
        case STATE_GEOMETRY:
            return dataHandlerGeometry(data, nLen);
            break;
        case STATE_IGNORED_FEATURE:
            return OGRERR_NONE;
            break;
        case STATE_BOUNDED_BY:
            return OGRERR_NONE;
            break;
        case STATE_BOUNDED_BY_IN_FEATURE:
            return dataHandlerGeometry(data, nLen);
            break;
        case STATE_CITYGML_ATTRIBUTE:
            return dataHandlerAttribute(data, nLen);
            break;
        default:
            return OGRERR_NONE;
            break;
    }
}

#define PUSH_STATE(val)                                                        \
    do                                                                         \
    {                                                                          \
        nStackDepth++;                                                         \
        CPLAssert(nStackDepth < STACK_SIZE);                                   \
        stateStack[nStackDepth] = val;                                         \
    } while (false)
#define POP_STATE() nStackDepth--

/************************************************************************/
/*                       startElementBoundedBy()                        */
/************************************************************************/

OGRErr GMLHandler::startElementBoundedBy(const char *pszName, int /*nLenName*/,
                                         void *attr)
{
    if (m_nDepth == 2 && strcmp(pszName, "Envelope") == 0)
    {
        char *pszGlobalSRSName = GetAttributeValue(attr, "srsName");
        m_poReader->SetGlobalSRSName(pszGlobalSRSName);
        CPLFree(pszGlobalSRSName);

        if (m_nSRSDimensionIfMissing == 0)
        {
            char *pszGlobalSRSDimension =
                GetAttributeValue(attr, "srsDimension");
            if (pszGlobalSRSDimension)
                m_nSRSDimensionIfMissing = atoi(pszGlobalSRSDimension);
            CPLFree(pszGlobalSRSDimension);
        }
    }

    return OGRERR_NONE;
}

/************************************************************************/
/*                       startElementGeometry()                         */
/************************************************************************/

OGRErr GMLHandler::startElementGeometry(const char *pszName, int nLenName,
                                        void *attr)
{
    if (stateStack[nStackDepth] == STATE_BOUNDED_BY_IN_FEATURE &&
        apsXMLNode.empty())
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Invalid <boundedBy> construct");
        return OGRERR_FAILURE;
    }

    /* Create new XML Element */
    CPLXMLNode *psCurNode =
        static_cast<CPLXMLNode *>(CPLCalloc(sizeof(CPLXMLNode), 1));
    psCurNode->eType = CXT_Element;
    psCurNode->pszValue = static_cast<char *>(CPLMalloc(nLenName + 1));
    memcpy(psCurNode->pszValue, pszName, nLenName + 1);

    /* Attach element as the last child of its parent */
    NodeLastChild &sNodeLastChild = apsXMLNode.back();
    CPLXMLNode *psLastChildParent = sNodeLastChild.psLastChild;

    if (psLastChildParent == nullptr)
    {
        CPLXMLNode *psParent = sNodeLastChild.psNode;
        if (psParent)
            psParent->psChild = psCurNode;
    }
    else
    {
        psLastChildParent->psNext = psCurNode;
    }
    sNodeLastChild.psLastChild = psCurNode;

    /* Add attributes to the element */
    CPLXMLNode *psLastChildCurNode = AddAttributes(psCurNode, attr);
    for (CPLXMLNode *psIter = psCurNode->psChild; psIter;
         psIter = psIter->psNext)
    {
        if (psIter->eType == CXT_Attribute &&
            strcmp(psIter->pszValue, "xlink:href") == 0 &&
            psIter->psChild->pszValue && psIter->psChild->pszValue[0] == '#')
        {
            m_oMapElementToSubstitute[psIter->psChild->pszValue + 1] =
                psCurNode;
        }
    }

    /* Some CityGML lack a srsDimension="3" in posList, such as in */
    /* http://www.citygml.org/fileadmin/count.php?f=fileadmin%2Fcitygml%2Fdocs%2FFrankfurt_Street_Setting_LOD3.zip
     */
    /* So we have to add it manually */
    if (strcmp(pszName, "posList") == 0 &&
        CPLGetXMLValue(psCurNode, "srsDimension", nullptr) == nullptr &&
        m_nSRSDimensionIfMissing != 0)
    {
        CPLXMLNode *psChild =
            CPLCreateXMLNode(nullptr, CXT_Attribute, "srsDimension");
        CPLCreateXMLNode(psChild, CXT_Text,
                         (m_nSRSDimensionIfMissing == 3) ? "3" : "2");

        if (psLastChildCurNode == nullptr)
            psCurNode->psChild = psChild;
        else
            psLastChildCurNode->psNext = psChild;
        psLastChildCurNode = psChild;
    }

    /* Push the element on the stack */
    NodeLastChild sNewNodeLastChild;
    sNewNodeLastChild.psNode = psCurNode;
    sNewNodeLastChild.psLastChild = psLastChildCurNode;
    apsXMLNode.push_back(sNewNodeLastChild);

    if (m_pszGeometry)
    {
        CPLFree(m_pszGeometry);
        m_pszGeometry = nullptr;
        m_nGeomAlloc = 0;
        m_nGeomLen = 0;
    }

    return OGRERR_NONE;
}

/************************************************************************/
/*                    startElementCityGMLGenericAttr()                  */
/************************************************************************/

OGRErr GMLHandler::startElementCityGMLGenericAttr(const char *pszName,
                                                  int /*nLenName*/,
                                                  void * /*attr*/)
{
    if (strcmp(pszName, "value") == 0)
    {
        if (m_pszCurField)
        {
            CPLFree(m_pszCurField);
            m_pszCurField = nullptr;
            m_nCurFieldLen = 0;
            m_nCurFieldAlloc = 0;
        }
        m_bInCurField = true;
    }

    return OGRERR_NONE;
}

/************************************************************************/
/*                       DealWithAttributes()                           */
/************************************************************************/

void GMLHandler::DealWithAttributes(const char *pszName, int nLenName,
                                    void *attr)
{
    GMLReadState *poState = m_poReader->GetState();
    GMLFeatureClass *poClass = poState->m_poFeature->GetClass();

    for (unsigned int idx = 0; true; idx++)
    {
        char *pszAttrKey = nullptr;

        char *pszAttrVal = GetAttributeByIdx(attr, idx, &pszAttrKey);
        if (pszAttrVal == nullptr)
            break;

        int nAttrIndex = 0;
        const char *pszAttrKeyNoNS = strchr(pszAttrKey, ':');
        if (pszAttrKeyNoNS != nullptr)
            pszAttrKeyNoNS++;

        /* If attribute is referenced by the .gfs */
        if (poClass->IsSchemaLocked() &&
            ((pszAttrKeyNoNS != nullptr &&
              (nAttrIndex = m_poReader->GetAttributeElementIndex(
                   pszName, nLenName, pszAttrKeyNoNS)) != -1) ||
             ((nAttrIndex = m_poReader->GetAttributeElementIndex(
                   pszName, nLenName, pszAttrKey)) != -1)))
        {
            nAttrIndex = FindRealPropertyByCheckingConditions(nAttrIndex, attr);
            if (nAttrIndex >= 0)
            {
                m_poReader->SetFeaturePropertyDirectly(nullptr, pszAttrVal,
                                                       nAttrIndex);
                pszAttrVal = nullptr;
            }
        }

        /* Hard-coded historic cases */
        else if (strcmp(pszAttrKey, "xlink:href") == 0)
        {
            if ((m_bReportHref || m_poReader->ReportAllAttributes()) &&
                m_bInCurField)
            {
                CPLFree(m_pszHref);
                m_pszHref = pszAttrVal;
                pszAttrVal = nullptr;
            }
            else if ((!poClass->IsSchemaLocked() &&
                      (m_bReportHref || m_poReader->ReportAllAttributes())) ||
                     (poClass->IsSchemaLocked() &&
                      (nAttrIndex = m_poReader->GetAttributeElementIndex(
                           (std::string(pszName) + "_href").c_str(),
                           nLenName + 5)) != -1))
            {
                poState->PushPath(pszName, nLenName);
                CPLString osPropNameHref = poState->osPath + "_href";
                poState->PopPath();
                m_poReader->SetFeaturePropertyDirectly(osPropNameHref,
                                                       pszAttrVal, nAttrIndex);
                pszAttrVal = nullptr;
            }
        }
        else if (strcmp(pszAttrKey, "uom") == 0)
        {
            CPLFree(m_pszUom);
            m_pszUom = pszAttrVal;
            pszAttrVal = nullptr;
        }
        else if (strcmp(pszAttrKey, "value") == 0)
        {
            CPLFree(m_pszValue);
            m_pszValue = pszAttrVal;
            pszAttrVal = nullptr;
        }
        else /* Get language in 'kieli' attribute of 'teksti' element */
            if (eAppSchemaType == APPSCHEMA_MTKGML && nLenName == 6 &&
                strcmp(pszName, "teksti") == 0 &&
                strcmp(pszAttrKey, "kieli") == 0)
            {
                CPLFree(m_pszKieli);
                m_pszKieli = pszAttrVal;
                pszAttrVal = nullptr;
            }

            /* Should we report all attributes ? */
            else if (m_poReader->ReportAllAttributes() &&
                     !poClass->IsSchemaLocked())
            {
                poState->PushPath(pszName, nLenName);
                CPLString osPropName = poState->osPath;
                poState->PopPath();

                m_poReader->SetFeaturePropertyDirectly(
                    CPLSPrintf("%s@%s", osPropName.c_str(),
                               pszAttrKeyNoNS ? pszAttrKeyNoNS : pszAttrKey),
                    pszAttrVal, -1);
                pszAttrVal = nullptr;
            }

        CPLFree(pszAttrKey);
        CPLFree(pszAttrVal);
    }

#if 0
    if( poClass->IsSchemaLocked() )
    {
        poState->PushPath( pszName, nLenName );
        CPLString osPath = poState->osPath;
        poState->PopPath();
        /* Find fields that match an attribute that is missing */
        for(int i=0; i < poClass->GetPropertyCount(); i++ )
        {
            GMLPropertyDefn* poProp = poClass->GetProperty(i);
            const char* pszSrcElement = poProp->GetSrcElement();
            if( poProp->GetType() == OFTStringList &&
                poProp->GetSrcElementLen() > osPath.size() &&
                strncmp(pszSrcElement, osPath, osPath.size()) == 0 &&
                pszSrcElement[osPath.size()] == '@' )
            {
                char* pszAttrVal = GetAttributeValue(attr, pszSrcElement + osPath.size() + 1);
                if( pszAttrVal == NULL )
                {
                    const char* pszCond = poProp->GetCondition();
                    if( pszCond == NULL || IsConditionMatched(pszCond, attr) )
                    {
                        m_poReader->SetFeaturePropertyDirectly( NULL, CPLStrdup(""), i );
                    }
                }
                else
                    CPLFree(pszAttrVal);
            }
        }
    }
#endif
}

/************************************************************************/
/*                        IsConditionMatched()                          */
/************************************************************************/

/* FIXME! 'and' / 'or' operators are evaluated left to right, without */
/* and precedence rules between them ! */

bool GMLHandler::IsConditionMatched(const char *pszCondition, void *attr)
{
    if (pszCondition == nullptr)
        return true;

    bool bSyntaxError = false;
    CPLString osCondAttr, osCondVal;
    const char *pszIter = pszCondition;
    bool bOpEqual = true;
    while (*pszIter == ' ')
        pszIter++;
    if (*pszIter != '@')
        bSyntaxError = true;
    else
    {
        pszIter++;
        while (*pszIter != '\0' && *pszIter != ' ' && *pszIter != '!' &&
               *pszIter != '=')
        {
            osCondAttr += *pszIter;
            pszIter++;
        }
        while (*pszIter == ' ')
            pszIter++;

        if (*pszIter == '!')
        {
            bOpEqual = false;
            pszIter++;
        }

        if (*pszIter != '=')
            bSyntaxError = true;
        else
        {
            pszIter++;
            while (*pszIter == ' ')
                pszIter++;
            if (*pszIter != '\'')
                bSyntaxError = true;
            else
            {
                pszIter++;
                while (*pszIter != '\0' && *pszIter != '\'')
                {
                    osCondVal += *pszIter;
                    pszIter++;
                }
                if (*pszIter != '\'')
                    bSyntaxError = true;
                else
                {
                    pszIter++;
                    while (*pszIter == ' ')
                        pszIter++;
                }
            }
        }
    }

    if (bSyntaxError)
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "Invalid condition : %s. Must be of the form "
                 "@attrname[!]='attrvalue' [and|or other_cond]*. "
                 "'and' and 'or' operators cannot be mixed",
                 pszCondition);
        return false;
    }

    char *pszVal = GetAttributeValue(attr, osCondAttr);
    if (pszVal == nullptr)
        pszVal = CPLStrdup("");
    const bool bCondMet = (bOpEqual && strcmp(pszVal, osCondVal) == 0) ||
                          (!bOpEqual && strcmp(pszVal, osCondVal) != 0);
    CPLFree(pszVal);
    if (*pszIter == '\0')
        return bCondMet;

    if (STARTS_WITH(pszIter, "and"))
    {
        pszIter += 3;
        if (!bCondMet)
            return false;
        return IsConditionMatched(pszIter, attr);
    }

    if (STARTS_WITH(pszIter, "or"))
    {
        pszIter += 2;
        if (bCondMet)
            return true;
        return IsConditionMatched(pszIter, attr);
    }

    CPLError(
        CE_Failure, CPLE_NotSupported,
        "Invalid condition : %s. Must be of the form @attrname[!]='attrvalue' "
        "[and|or other_cond]*. 'and' and 'or' operators cannot be mixed",
        pszCondition);
    return false;
}

/************************************************************************/
/*                FindRealPropertyByCheckingConditions()                */
/************************************************************************/

int GMLHandler::FindRealPropertyByCheckingConditions(int nIdx, void *attr)
{
    GMLReadState *poState = m_poReader->GetState();
    GMLFeatureClass *poClass = poState->m_poFeature->GetClass();

    GMLPropertyDefn *poProp = poClass->GetProperty(nIdx);
    const char *pszCond = poProp->GetCondition();
    if (pszCond != nullptr && !IsConditionMatched(pszCond, attr))
    {
        /* try other attributes with same source element, but with different */
        /* condition */
        const char *pszSrcElement = poProp->GetSrcElement();
        nIdx = -1;
        for (int i = m_nAttributeIndex + 1; i < poClass->GetPropertyCount();
             i++)
        {
            poProp = poClass->GetProperty(i);
            if (strcmp(poProp->GetSrcElement(), pszSrcElement) == 0)
            {
                pszCond = poProp->GetCondition();
                if (IsConditionMatched(pszCond, attr))
                {
                    nIdx = i;
                    break;
                }
            }
        }
    }
    return nIdx;
}

/************************************************************************/
/*                      startElementFeatureAttribute()                  */
/************************************************************************/

OGRErr GMLHandler::startElementFeatureAttribute(const char *pszName,
                                                int nLenName, void *attr)
{
    /* Reset flag */
    m_bInCurField = false;

    GMLReadState *poState = m_poReader->GetState();

    /* -------------------------------------------------------------------- */
    /*      If we are collecting geometry, or if we determine this is a     */
    /*      geometry element then append to the geometry info.              */
    /* -------------------------------------------------------------------- */
    if (IsGeometryElement(pszName))
    {
        bool bReadGeometry;

        /* If the <GeometryElementPath> is defined in the .gfs, use it */
        /* to read the appropriate geometry element */
        GMLFeatureClass *poClass = poState->m_poFeature->GetClass();
        m_nGeometryPropertyIndex = 0;
        if (poClass->IsSchemaLocked() &&
            poClass->GetGeometryPropertyCount() == 0)
        {
            bReadGeometry = false;
        }
        else if (poClass->IsSchemaLocked() &&
                 poClass->GetGeometryPropertyCount() == 1 &&
                 poClass->GetGeometryProperty(0)->GetSrcElement()[0] == '\0')
        {
            bReadGeometry = true;
        }
        else if (poClass->IsSchemaLocked() &&
                 poClass->GetGeometryPropertyCount() > 0)
        {
            m_nGeometryPropertyIndex =
                poClass->GetGeometryPropertyIndexBySrcElement(
                    poState->osPath.c_str());
            bReadGeometry = (m_nGeometryPropertyIndex >= 0);
        }
        else if (m_poReader->FetchAllGeometries())
        {
            bReadGeometry = true;
        }
        else if (!poClass->IsSchemaLocked() && m_poReader->IsWFSJointLayer())
        {
            m_nGeometryPropertyIndex =
                poClass->GetGeometryPropertyIndexBySrcElement(
                    poState->osPath.c_str());
            if (m_nGeometryPropertyIndex < 0)
            {
                const char *pszElement = poState->osPath.c_str();
                CPLString osFieldName;
                /* Strip member| prefix. Should always be true normally */
                if (STARTS_WITH(pszElement, "member|"))
                    osFieldName = pszElement + strlen("member|");

                /* Replace layer|property by layer_property */
                size_t iPos = osFieldName.find('|');
                if (iPos != std::string::npos)
                    osFieldName[iPos] = '.';

                poClass->AddGeometryProperty(new GMLGeometryPropertyDefn(
                    osFieldName, poState->osPath.c_str(), wkbUnknown, -1,
                    true));
                m_nGeometryPropertyIndex = poClass->GetGeometryPropertyCount();
            }
            bReadGeometry = true;
        }
        else
        {
            /* AIXM special case: for RouteSegment, we only want to read Curve
             * geometries */
            /* not 'start' and 'end' geometries */
            if (eAppSchemaType == APPSCHEMA_AIXM &&
                strcmp(poState->m_poFeature->GetClass()->GetName(),
                       "RouteSegment") == 0)
                bReadGeometry = strcmp(pszName, "Curve") == 0;

            // AIXM special case: we want to read both horizontalProjection_location and
            // horizontalProjection_linearExtent
            else if (eAppSchemaType == APPSCHEMA_AIXM &&
                     STARTS_WITH(poState->m_poFeature->GetClass()->GetName(),
                                 "horizontalProjection_") == 0)
                bReadGeometry =
                    STARTS_WITH(pszName, "horizontalProjection_") == 0;

            /* For Inspire objects : the "main" geometry is in a <geometry>
             * element */
            else if (m_bAlreadyFoundGeometry)
                bReadGeometry = false;
            else if (strcmp(poState->osPath.c_str(), "geometry") == 0)
            {
                m_bAlreadyFoundGeometry = true;
                bReadGeometry = true;
                m_nGeometryPropertyIndex =
                    poClass->GetGeometryPropertyIndexBySrcElement(
                        poState->osPath.c_str());
                if (m_nGeometryPropertyIndex < 0)
                {
                    poClass->AddGeometryProperty(new GMLGeometryPropertyDefn(
                        "geometry", poState->osPath.c_str(), wkbUnknown, -1,
                        true));
                    m_nGeometryPropertyIndex =
                        poClass->GetGeometryPropertyCount();
                }
            }
            else
            {
                if (!poClass->IsSchemaLocked() &&
                    poClass->IsConsistentSingleGeomElemPath())
                {
                    const std::string &osGeomElemPath =
                        poClass->GetSingleGeomElemPath();
                    if (osGeomElemPath.empty())
                    {
                        poClass->SetSingleGeomElemPath(poState->osPath);
                    }
                    else if (poState->osPath != osGeomElemPath)
                    {
                        poClass->SetConsistentSingleGeomElemPath(false);
                        poClass->SetSingleGeomElemPath(std::string());
                    }
                }
                bReadGeometry = true;
            }
        }
        if (bReadGeometry)
        {
            m_nGeometryDepth = m_nDepth;

            CPLAssert(apsXMLNode.empty());

            NodeLastChild sNodeLastChild;
            sNodeLastChild.psNode = nullptr;
            sNodeLastChild.psLastChild = nullptr;
            apsXMLNode.push_back(sNodeLastChild);

            PUSH_STATE(STATE_GEOMETRY);

            return startElementGeometry(pszName, nLenName, attr);
        }
    }
    else if (
        (nLenName == 9 || nLenName == 8) &&
        (strcmp(pszName, "boundedBy") == 0 ||
         strcmp(pszName, "boundary") == 0) &&
        // We ignore the UseBBOX() flag for CityGML, since CityGML
        // has elements like bldg:boundedBy or 'boundary', which are not a simple
        // rectangular bbox. This is needed to read properly
        // autotest/ogr/data/gml/citygml_lod2_713_5322.xml
        // (this is a workaround of not being namespace aware)
        (eAppSchemaType == APPSCHEMA_CITYGML || m_poReader->UseBBOX()))
    {
        m_inBoundedByDepth = m_nDepth;

        CPLAssert(apsXMLNode.empty());

        NodeLastChild sNodeLastChild;
        sNodeLastChild.psNode = nullptr;
        sNodeLastChild.psLastChild = nullptr;
        apsXMLNode.push_back(sNodeLastChild);

        PUSH_STATE(STATE_BOUNDED_BY_IN_FEATURE);

        return OGRERR_NONE;
    }

    /* -------------------------------------------------------------------- */
    /*      Is it a CityGML generic attribute ?                             */
    /* -------------------------------------------------------------------- */
    else if (eAppSchemaType == APPSCHEMA_CITYGML &&
             m_poReader->IsCityGMLGenericAttributeElement(pszName, attr))
    {
        CPLFree(m_pszCityGMLGenericAttrName);
        m_pszCityGMLGenericAttrName = GetAttributeValue(attr, "name");
        m_inCityGMLGenericAttrDepth = m_nDepth;

        PUSH_STATE(STATE_CITYGML_ATTRIBUTE);

        return OGRERR_NONE;
    }

    else if (m_poReader->IsWFSJointLayer() && m_nDepth == m_nDepthFeature + 1)
    {
    }

    else if (m_poReader->IsWFSJointLayer() && m_nDepth == m_nDepthFeature + 2)
    {
        const char *pszFID = GetFID(attr);
        if (pszFID)
        {
            poState->PushPath(pszName, nLenName);
            CPLString osPropPath = poState->osPath + "@id";
            poState->PopPath();
            m_poReader->SetFeaturePropertyDirectly(osPropPath,
                                                   CPLStrdup(pszFID), -1);
        }
    }

    /* -------------------------------------------------------------------- */
    /*      If it is (or at least potentially is) a simple attribute,       */
    /*      then start collecting it.                                       */
    /* -------------------------------------------------------------------- */
    else if ((m_nAttributeIndex = m_poReader->GetAttributeElementIndex(
                  pszName, nLenName)) != -1)
    {
        GMLFeatureClass *poClass = poState->m_poFeature->GetClass();
        if (poClass->IsSchemaLocked() &&
            (poClass->GetProperty(m_nAttributeIndex)->GetType() ==
                 GMLPT_FeatureProperty ||
             poClass->GetProperty(m_nAttributeIndex)->GetType() ==
                 GMLPT_FeaturePropertyList))
        {
            m_nAttributeDepth = m_nDepth;
            PUSH_STATE(STATE_FEATUREPROPERTY);
        }
        else
        {
            /* Is this a property with a condition on an attribute value ? */
            if (poClass->IsSchemaLocked())
            {
                m_nAttributeIndex = FindRealPropertyByCheckingConditions(
                    m_nAttributeIndex, attr);
            }

            if (m_nAttributeIndex >= 0)
            {
                if (m_pszCurField)
                {
                    CPLFree(m_pszCurField);
                    m_pszCurField = nullptr;
                    m_nCurFieldLen = 0;
                    m_nCurFieldAlloc = 0;
                }
                m_bInCurField = true;

                char *pszXSINil = GetAttributeValue(attr, "xsi:nil");
                if (pszXSINil)
                {
                    if (EQUAL(pszXSINil, "true"))
                        m_poReader->SetFeaturePropertyDirectly(
                            pszName, CPLStrdup(OGR_GML_NULL), -1);
                    CPLFree(pszXSINil);
                }
                else
                {
                    DealWithAttributes(pszName, nLenName, attr);
                }

                if (stateStack[nStackDepth] != STATE_PROPERTY)
                {
                    m_nAttributeDepth = m_nDepth;
                    PUSH_STATE(STATE_PROPERTY);
                }
            }
            /*else
            {
                DealWithAttributes(pszName, nLenName, attr);
            }*/
        }
    }
    else
    {
        DealWithAttributes(pszName, nLenName, attr);
    }

    poState->PushPath(pszName, nLenName);

    return OGRERR_NONE;
}

/************************************************************************/
/*                         startElementTop()                            */
/************************************************************************/

OGRErr GMLHandler::startElementTop(const char *pszName, int /*nLenName*/,
                                   void *attr)
{
    if (strcmp(pszName, "CityModel") == 0)
    {
        eAppSchemaType = APPSCHEMA_CITYGML;
        // Default to 3D geometries for CityGML (#6989)
        if (m_nSRSDimensionIfMissing == 0)
            m_nSRSDimensionIfMissing = 3;
    }
    else if (strcmp(pszName, "AIXMBasicMessage") == 0)
    {
        eAppSchemaType = APPSCHEMA_AIXM;
        m_bReportHref = true;
    }
    else if (strcmp(pszName, "Maastotiedot") == 0)
    {
        eAppSchemaType = APPSCHEMA_MTKGML;

        char *pszSRSName = GetAttributeValue(attr, "srsName");
        m_poReader->SetGlobalSRSName(pszSRSName);
        CPLFree(pszSRSName);

        m_bReportHref = true;

        /* the schemas of MTKGML don't have (string) width, so don't set it */
        m_poReader->SetWidthFlag(false);
    }

    stateStack[0] = STATE_DEFAULT;

    return OGRERR_NONE;
}

/************************************************************************/
/*                        startElementDefault()                         */
/************************************************************************/

OGRErr GMLHandler::startElementDefault(const char *pszName, int nLenName,
                                       void *attr)

{
    /* -------------------------------------------------------------------- */
    /*      Is it a feature?  If so push a whole new state, and return.     */
    /* -------------------------------------------------------------------- */
    int nClassIndex;
    const char *pszFilteredClassName = nullptr;

    if (nLenName == 9 && strcmp(pszName, "boundedBy") == 0)
    {
        m_inBoundedByDepth = m_nDepth;

        PUSH_STATE(STATE_BOUNDED_BY);

        return OGRERR_NONE;
    }

    else if (m_poReader->ShouldLookForClassAtAnyLevel() &&
             (pszFilteredClassName = m_poReader->GetFilteredClassName()) !=
                 nullptr)
    {
        if (strcmp(pszName, pszFilteredClassName) == 0)
        {
            m_poReader->PushFeature(pszName, GetFID(attr),
                                    m_poReader->GetFilteredClassIndex());

            m_nDepthFeature = m_nDepth;

            PUSH_STATE(STATE_FEATURE);

            return OGRERR_NONE;
        }
    }

    /* WFS 2.0 GetFeature documents have a wfs:FeatureCollection */
    /* as a wfs:member of the top wfs:FeatureCollection. We don't want this */
    /* wfs:FeatureCollection to be recognized as a feature */
    else if ((!(nLenName == static_cast<int>(strlen("FeatureCollection")) &&
                strcmp(pszName, "FeatureCollection") == 0)) &&
             (nClassIndex = m_poReader->GetFeatureElementIndex(
                  pszName, nLenName, eAppSchemaType)) != -1)
    {
        m_bAlreadyFoundGeometry = false;

        pszFilteredClassName = m_poReader->GetFilteredClassName();
        if (pszFilteredClassName != nullptr &&
            strcmp(pszName, pszFilteredClassName) != 0)
        {
            m_nDepthFeature = m_nDepth;

            PUSH_STATE(STATE_IGNORED_FEATURE);

            return OGRERR_NONE;
        }
        else
        {
            if (eAppSchemaType == APPSCHEMA_MTKGML)
            {
                m_poReader->PushFeature(pszName, nullptr, nClassIndex);

                char *pszGID = GetAttributeValue(attr, "gid");
                if (pszGID)
                    m_poReader->SetFeaturePropertyDirectly("gid", pszGID, -1,
                                                           GMLPT_String);
            }
            else
                m_poReader->PushFeature(pszName, GetFID(attr), nClassIndex);

            m_nDepthFeature = m_nDepth;

            PUSH_STATE(STATE_FEATURE);

            return OGRERR_NONE;
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Push the element onto the current state's path.                 */
    /* -------------------------------------------------------------------- */
    m_poReader->GetState()->PushPath(pszName, nLenName);

    return OGRERR_NONE;
}

/************************************************************************/
/*                      endElementIgnoredFeature()                      */
/************************************************************************/

OGRErr GMLHandler::endElementIgnoredFeature()

{
    if (m_nDepth == m_nDepthFeature)
    {
        POP_STATE();
    }
    return OGRERR_NONE;
}

/************************************************************************/
/*                         endElementBoundedBy()                        */
/************************************************************************/
OGRErr GMLHandler::endElementBoundedBy()

{
    if (m_inBoundedByDepth == m_nDepth)
    {
        POP_STATE();
    }

    return OGRERR_NONE;
}

/************************************************************************/
/*                     endElementBoundedByInFeature()                   */
/************************************************************************/
OGRErr GMLHandler::endElementBoundedByInFeature()

{
    if (m_nDepth > m_inBoundedByDepth)
    {
        if (m_nDepth == m_inBoundedByDepth + 1)
        {
            m_nGeometryDepth = m_nDepth;
        }
        return endElementGeometry();
    }
    else
    {
        POP_STATE();
        if (apsXMLNode.size() >= 2 && apsXMLNode[1].psNode != nullptr)
            CPLDestroyXMLNode(apsXMLNode[1].psNode);
        apsXMLNode.clear();
        return OGRERR_NONE;
    }
}

/************************************************************************/
/*                      ParseAIXMElevationProperties()                  */
/************************************************************************/

void GMLHandler::ParseAIXMElevationProperties(const CPLXMLNode *psGML)
{
    if (const char *pszElevation = CPLGetXMLValue(psGML, "elevation", nullptr))
    {
        m_poReader->SetFeaturePropertyDirectly("elevation",
                                               CPLStrdup(pszElevation), -1);
        const char *pszElevationUnit =
            CPLGetXMLValue(psGML, "elevation.uom", nullptr);
        if (pszElevationUnit)
        {
            m_poReader->SetFeaturePropertyDirectly(
                "elevation_uom", CPLStrdup(pszElevationUnit), -1);
        }
    }

    if (const char *pszGeoidUndulation =
            CPLGetXMLValue(psGML, "geoidUndulation", nullptr))
    {
        m_poReader->SetFeaturePropertyDirectly(
            "geoidUndulation", CPLStrdup(pszGeoidUndulation), -1);
        const char *pszGeoidUndulationUnit =
            CPLGetXMLValue(psGML, "geoidUndulation.uom", nullptr);
        if (pszGeoidUndulationUnit)
        {
            m_poReader->SetFeaturePropertyDirectly(
                "geoidUndulation_uom", CPLStrdup(pszGeoidUndulationUnit), -1);
        }
    }

    if (const char *pszVerticalDatum =
            CPLGetXMLValue(psGML, "verticalDatum", nullptr))
    {
        m_poReader->SetFeaturePropertyDirectly("verticalDatum",
                                               CPLStrdup(pszVerticalDatum), -1);
    }

    if (const char *pszVerticalAccuracy =
            CPLGetXMLValue(psGML, "verticalAccuracy", nullptr))
    {
        m_poReader->SetFeaturePropertyDirectly(
            "verticalAccuracy", CPLStrdup(pszVerticalAccuracy), -1);
        const char *pszVerticalAccuracyUnit =
            CPLGetXMLValue(psGML, "verticalAccuracy.uom", nullptr);
        if (pszVerticalAccuracyUnit)
        {
            m_poReader->SetFeaturePropertyDirectly(
                "verticalAccuracy_uom", CPLStrdup(pszVerticalAccuracyUnit), -1);
        }
    }
}

/************************************************************************/
/*                       ParseAIXMElevationPoint()                      */
/************************************************************************/

CPLXMLNode *GMLHandler::ParseAIXMElevationPoint(CPLXMLNode *psGML)
{
    ParseAIXMElevationProperties(psGML);

    const char *pszPos = CPLGetXMLValue(psGML, "pos", nullptr);
    const char *pszCoordinates = CPLGetXMLValue(psGML, "coordinates", nullptr);
    if (pszPos != nullptr || pszCoordinates != nullptr)
    {
        CPLFree(psGML->pszValue);
        psGML->pszValue = CPLStrdup("gml:Point");
    }
    else
    {
        CPLDestroyXMLNode(psGML);
        psGML = nullptr;
    }

    return psGML;
}

/************************************************************************/
/*                         endElementGeometry()                         */
/************************************************************************/
OGRErr GMLHandler::endElementGeometry()

{
    if (m_nGeomLen)
    {
        CPLXMLNode *psNode =
            static_cast<CPLXMLNode *>(CPLCalloc(sizeof(CPLXMLNode), 1));
        psNode->eType = CXT_Text;
        psNode->pszValue = m_pszGeometry;

        NodeLastChild &sNodeLastChild = apsXMLNode.back();
        CPLXMLNode *psLastChildParent = sNodeLastChild.psLastChild;
        if (psLastChildParent == nullptr)
        {
            CPLXMLNode *psParent = sNodeLastChild.psNode;
            if (psParent)
                psParent->psChild = psNode;
        }
        else
            psLastChildParent->psNext = psNode;
        sNodeLastChild.psLastChild = psNode;

        m_pszGeometry = nullptr;
        m_nGeomAlloc = 0;
        m_nGeomLen = 0;
    }

    CPLXMLNode *psThisNode = apsXMLNode.back().psNode;
    CPLXMLNode *psThisNodeChild = psThisNode->psChild;
    if (!m_oMapElementToSubstitute.empty() && psThisNodeChild &&
        psThisNodeChild->eType == CXT_Attribute &&
        strcmp(psThisNodeChild->pszValue, "gml:id") == 0 &&
        psThisNodeChild->psChild->pszValue)
    {
        auto oIter =
            m_oMapElementToSubstitute.find(psThisNodeChild->psChild->pszValue);
        if (oIter != m_oMapElementToSubstitute.end())
        {
            auto psLastChild = oIter->second->psChild;
            if (psLastChild)
            {
                // CPLDebug("GML", "Substitution of xlink:href=\"#%s\" with actual content", psThisNodeChild->psChild->pszValue);
                CPLXMLNode *psAfter = psThisNode->psNext;
                psThisNode->psNext = nullptr;
                // We can patch oIter->second as it stored as it in the current
                // GMLFeature.
                // Of course that would no longer be the case in case of
                // cross-references between different GMLFeature, hence we clear
                // m_oMapElementToSubstitute at the end of the current feature.
                while (psLastChild->psNext)
                    psLastChild = psLastChild->psNext;
                if (psLastChild == psThisNode)
                {
                    /* Can happen in situations like:
                     <foo xlink:href="#X">
                        <bar gml:id="X"/>
                     </foo>
                     Do not attempt substitution as that would cause a memory
                     leak.
                    */
                }
                else
                {
                    psLastChild->psNext = CPLCloneXMLTree(psThisNode);
                }
                psThisNode->psNext = psAfter;
            }
        }
    }

    if (m_nDepth == m_nGeometryDepth)
    {
        m_nGeometryDepth = 0;

        CPLAssert(apsXMLNode.size() == 2);
        CPLXMLNode *psInterestNode = apsXMLNode.back().psNode;

        /*char* pszXML = CPLSerializeXMLTree(psInterestNode);
        CPLDebug("GML", "geometry = %s", pszXML);
        CPLFree(pszXML);*/

        apsXMLNode.pop_back();

        /* AIXM ElevatedPoint. We want to parse this */
        /* a bit specially because ElevatedPoint is aixm: stuff and */
        /* the srsDimension of the <gml:pos> can be set to true although */
        /* they are only 2 coordinates in practice */
        if (eAppSchemaType == APPSCHEMA_AIXM && psInterestNode != nullptr &&
            strcmp(psInterestNode->pszValue, "ElevatedPoint") == 0)
        {
            psInterestNode = ParseAIXMElevationPoint(psInterestNode);
        }
        else if (eAppSchemaType == APPSCHEMA_AIXM &&
                 psInterestNode != nullptr &&
                 (strcmp(psInterestNode->pszValue, "ElevatedCurve") == 0 ||
                  strcmp(psInterestNode->pszValue, "ElevateSurface") == 0))
        {
            ParseAIXMElevationProperties(psInterestNode);
        }
        else if (eAppSchemaType == APPSCHEMA_MTKGML &&
                 psInterestNode != nullptr)
        {
            if (strcmp(psInterestNode->pszValue, "Murtoviiva") == 0)
            {
                CPLFree(psInterestNode->pszValue);
                psInterestNode->pszValue = CPLStrdup("gml:LineString");
            }
            else if (strcmp(psInterestNode->pszValue, "Alue") == 0)
            {
                CPLFree(psInterestNode->pszValue);
                psInterestNode->pszValue = CPLStrdup("gml:Polygon");
            }
            else if (strcmp(psInterestNode->pszValue, "Piste") == 0)
            {
                CPLFree(psInterestNode->pszValue);
                psInterestNode->pszValue = CPLStrdup("gml:Point");
            }
        }
        else if (psInterestNode != nullptr &&
                 strcmp(psInterestNode->pszValue, "BoundingBox") == 0)
        {
            CPLFree(psInterestNode->pszValue);
            psInterestNode->pszValue = CPLStrdup("Envelope");
            for (CPLXMLNode *psChild = psInterestNode->psChild; psChild;
                 psChild = psChild->psNext)
            {
                if (psChild->eType == CXT_Attribute &&
                    strcmp(psChild->pszValue, "crs") == 0)
                {
                    CPLFree(psChild->pszValue);
                    psChild->pszValue = CPLStrdup("srsName");
                    break;
                }
            }
        }

        GMLFeature *poGMLFeature = m_poReader->GetState()->m_poFeature;
        if (stateStack[nStackDepth] == STATE_BOUNDED_BY_IN_FEATURE)
        {
            if (eAppSchemaType == APPSCHEMA_CITYGML)
                CPLDestroyXMLNode(psInterestNode);
            else
                poGMLFeature->SetBoundedByGeometry(psInterestNode);
        }
        else
        {
            if (m_poReader->FetchAllGeometries())
                poGMLFeature->AddGeometry(psInterestNode);
            else
            {
                GMLFeatureClass *poClass = poGMLFeature->GetClass();
                if (poClass->GetGeometryPropertyCount() > 1)
                {
                    if (poGMLFeature->GetGeometryRef(m_nGeometryPropertyIndex))
                    {
                        // If we have already a geometry, setting a new one
                        // will invalidate nodes potentially stored in
                        // m_oMapElementToSubstitute, so clear it
                        m_oMapElementToSubstitute.clear();
                    }
                    poGMLFeature->SetGeometryDirectly(m_nGeometryPropertyIndex,
                                                      psInterestNode);
                }
                else
                {
                    if (poGMLFeature->GetGeometryRef(0))
                    {
                        // If we have already a geometry, setting a new one
                        // will invalidate nodes potentially stored in
                        // m_oMapElementToSubstitute, so clear it
                        m_oMapElementToSubstitute.clear();
                    }
                    poGMLFeature->SetGeometryDirectly(psInterestNode);
                }
            }

            POP_STATE();
        }
    }

    apsXMLNode.pop_back();

    return OGRERR_NONE;
}

/************************************************************************/
/*                    endElementCityGMLGenericAttr()                    */
/************************************************************************/
OGRErr GMLHandler::endElementCityGMLGenericAttr()

{
    if (m_pszCityGMLGenericAttrName != nullptr && m_bInCurField)
    {
        if (m_pszCurField != nullptr)
        {
            m_poReader->SetFeaturePropertyDirectly(m_pszCityGMLGenericAttrName,
                                                   m_pszCurField, -1);
        }
        m_pszCurField = nullptr;
        m_nCurFieldLen = 0;
        m_nCurFieldAlloc = 0;
        m_bInCurField = false;
        CPLFree(m_pszCityGMLGenericAttrName);
        m_pszCityGMLGenericAttrName = nullptr;
    }

    if (m_inCityGMLGenericAttrDepth == m_nDepth)
    {
        POP_STATE();
    }

    return OGRERR_NONE;
}

/************************************************************************/
/*                        endElementAttribute()                         */
/************************************************************************/
OGRErr GMLHandler::endElementAttribute()

{
    GMLReadState *poState = m_poReader->GetState();

    if (m_bInCurField)
    {
        if (m_pszCurField == nullptr && m_poReader->IsEmptyAsNull())
        {
            if (m_pszValue != nullptr)
            {
                m_poReader->SetFeaturePropertyDirectly(poState->osPath.c_str(),
                                                       m_pszValue, -1);
                m_pszValue = nullptr;
            }
        }
        else
        {
            m_poReader->SetFeaturePropertyDirectly(
                poState->osPath.c_str(),
                m_pszCurField ? m_pszCurField : CPLStrdup(""),
                m_nAttributeIndex);
            m_pszCurField = nullptr;
        }

        if (m_pszHref != nullptr)
        {
            CPLString osPropNameHref = poState->osPath + "_href";
            m_poReader->SetFeaturePropertyDirectly(osPropNameHref, m_pszHref,
                                                   -1);
            m_pszHref = nullptr;
        }

        if (m_pszUom != nullptr)
        {
            CPLString osPropNameUom = poState->osPath + "_uom";
            m_poReader->SetFeaturePropertyDirectly(osPropNameUom, m_pszUom, -1);
            m_pszUom = nullptr;
        }

        if (m_pszKieli != nullptr)
        {
            CPLString osPropName = poState->osPath + "_kieli";
            m_poReader->SetFeaturePropertyDirectly(osPropName, m_pszKieli, -1);
            m_pszKieli = nullptr;
        }

        m_nCurFieldLen = 0;
        m_nCurFieldAlloc = 0;
        m_bInCurField = false;
        m_nAttributeIndex = -1;

        CPLFree(m_pszValue);
        m_pszValue = nullptr;
    }

    poState->PopPath();

    if (m_nAttributeDepth == m_nDepth)
    {
        POP_STATE();
    }

    return OGRERR_NONE;
}

/************************************************************************/
/*                    startElementFeatureProperty()                     */
/************************************************************************/

OGRErr GMLHandler::startElementFeatureProperty(const char * /*pszName*/,
                                               int /*nLenName*/, void *attr)
{
    if (m_nDepth == m_nAttributeDepth + 1)
    {
        const char *pszGMLId = GetFID(attr);
        if (pszGMLId != nullptr)
        {
            m_poReader->SetFeaturePropertyDirectly(
                nullptr, CPLStrdup(CPLSPrintf("#%s", pszGMLId)),
                m_nAttributeIndex);
        }
    }

    return OGRERR_NONE;
}

/************************************************************************/
/*                      endElementFeatureProperty()                      */
/************************************************************************/

OGRErr GMLHandler::endElementFeatureProperty()

{
    if (m_nDepth == m_nAttributeDepth)
    {
        GMLReadState *poState = m_poReader->GetState();
        poState->PopPath();

        POP_STATE();
    }
    return OGRERR_NONE;
}

/************************************************************************/
/*                          endElementFeature()                         */
/************************************************************************/
OGRErr GMLHandler::endElementFeature()

{
    /* -------------------------------------------------------------------- */
    /*      If we are collecting a feature, and this element tag matches    */
    /*      element name for the class, then we have finished the           */
    /*      feature, and we pop the feature read state.                     */
    /* -------------------------------------------------------------------- */
    if (m_nDepth == m_nDepthFeature)
    {
        m_oMapElementToSubstitute.clear();
        m_poReader->PopState();

        POP_STATE();
    }

    /* -------------------------------------------------------------------- */
    /*      Otherwise, we just pop the element off the local read states    */
    /*      element stack.                                                  */
    /* -------------------------------------------------------------------- */
    else
    {
        m_poReader->GetState()->PopPath();
    }

    return OGRERR_NONE;
}

/************************************************************************/
/*                          endElementDefault()                         */
/************************************************************************/
OGRErr GMLHandler::endElementDefault()

{
    if (m_nDepth > 0)
        m_poReader->GetState()->PopPath();

    return OGRERR_NONE;
}

/************************************************************************/
/*                         dataHandlerAttribute()                       */
/************************************************************************/

OGRErr GMLHandler::dataHandlerAttribute(const char *data, int nLen)

{
    if (!m_bInCurField)
        return OGRERR_NONE;

    int nIter = 0;

    // Ignore white space.
    if (m_nCurFieldLen == 0)
    {
        while (nIter < nLen)
        {
            const char ch = data[nIter];
            if (!(ch == ' ' || ch == 10 || ch == 13 || ch == '\t'))
                break;
            nIter++;
        }
    }

    const int nCharsLen = nLen - nIter;

    if (nCharsLen > INT_MAX - static_cast<int>(m_nCurFieldLen) - 1)
    {
        CPLError(CE_Failure, CPLE_OutOfMemory,
                 "Too much data in a single element");
        return OGRERR_NOT_ENOUGH_MEMORY;
    }
    if (m_nCurFieldLen + nCharsLen + 1 > m_nCurFieldAlloc)
    {
        if (m_nCurFieldAlloc < INT_MAX - m_nCurFieldAlloc / 3 - nCharsLen - 1)
            m_nCurFieldAlloc =
                m_nCurFieldAlloc + m_nCurFieldAlloc / 3 + nCharsLen + 1;
        else
            m_nCurFieldAlloc = m_nCurFieldLen + nCharsLen + 1;
        char *pszNewCurField = static_cast<char *>(
            VSI_REALLOC_VERBOSE(m_pszCurField, m_nCurFieldAlloc));
        if (pszNewCurField == nullptr)
        {
            return OGRERR_NOT_ENOUGH_MEMORY;
        }
        m_pszCurField = pszNewCurField;
    }
    memcpy(m_pszCurField + m_nCurFieldLen, data + nIter, nCharsLen);
    m_nCurFieldLen += nCharsLen;
    m_pszCurField[m_nCurFieldLen] = '\0';

    return OGRERR_NONE;
}

/************************************************************************/
/*                         dataHandlerGeometry()                        */
/************************************************************************/

OGRErr GMLHandler::dataHandlerGeometry(const char *data, int nLen)

{
    int nIter = 0;

    // Ignore white space
    if (m_nGeomLen == 0)
    {
        while (nIter < nLen)
        {
            char ch = data[nIter];
            if (!(ch == ' ' || ch == 10 || ch == 13 || ch == '\t'))
                break;
            nIter++;
        }
    }

    const int nCharsLen = nLen - nIter;
    if (nCharsLen)
    {
        if (nCharsLen > INT_MAX - static_cast<int>(m_nGeomLen) - 1)
        {
            CPLError(CE_Failure, CPLE_OutOfMemory,
                     "Too much data in a single element");
            return OGRERR_NOT_ENOUGH_MEMORY;
        }
        if (m_nGeomLen + nCharsLen + 1 > m_nGeomAlloc)
        {
            if (m_nGeomAlloc < INT_MAX - m_nGeomAlloc / 3 - nCharsLen - 1)
                m_nGeomAlloc = m_nGeomAlloc + m_nGeomAlloc / 3 + nCharsLen + 1;
            else
                m_nGeomAlloc = m_nGeomAlloc + nCharsLen + 1;
            char *pszNewGeometry = static_cast<char *>(
                VSI_REALLOC_VERBOSE(m_pszGeometry, m_nGeomAlloc));
            if (pszNewGeometry == nullptr)
            {
                return OGRERR_NOT_ENOUGH_MEMORY;
            }
            m_pszGeometry = pszNewGeometry;
        }
        memcpy(m_pszGeometry + m_nGeomLen, data + nIter, nCharsLen);
        m_nGeomLen += nCharsLen;
        m_pszGeometry[m_nGeomLen] = '\0';
    }

    return OGRERR_NONE;
}

/************************************************************************/
/*                         IsGeometryElement()                          */
/************************************************************************/

bool GMLHandler::IsGeometryElement(const char *pszElement)

{
    int nFirst = 0;
    int nLast = GML_GEOMETRY_TYPE_COUNT - 1;
    unsigned long nHash = CPLHashSetHashStr(pszElement);
    do
    {
        const int nMiddle = (nFirst + nLast) / 2;
        if (nHash == pasGeometryNames[nMiddle].nHash)
            return strcmp(pszElement, pasGeometryNames[nMiddle].pszName) == 0;
        if (nHash < pasGeometryNames[nMiddle].nHash)
            nLast = nMiddle - 1;
        else
            nFirst = nMiddle + 1;
    } while (nFirst <= nLast);

    if (eAppSchemaType == APPSCHEMA_AIXM &&
        (strcmp(pszElement, "ElevatedPoint") == 0 ||
         strcmp(pszElement, "ElevatedCurve") == 0 ||
         strcmp(pszElement, "ElevatedSurface") == 0))
    {
        return true;
    }

    if (eAppSchemaType == APPSCHEMA_MTKGML &&
        (strcmp(pszElement, "Piste") == 0 || strcmp(pszElement, "Alue") == 0 ||
         strcmp(pszElement, "Murtoviiva") == 0))
        return true;

    return false;
}
