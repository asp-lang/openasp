// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#include "builtin_internal.h"
#include "component_internal.h"

#include <limits.h>
#include <libxml/parser.h>
#include <libxml/xmlsave.h>
#include <libxml/xpath.h>
#include <stdlib.h>
#include <string.h>

static oasp_value xml_slice_value(oasp_runtime *runtime, const char *data, size_t length) {
    char *copy = oasp_runtime_copy(runtime, data, length);
    return copy ? oasp_value_string(copy, length) : oasp_value_empty();
}

static void xml_finalize(void *context) {
    oasp_xml_document *document = context;
    if (document->document)
        xmlFreeDoc(document->document);
    document->document = NULL;
}

static void xml_node_finalize(void *context) {
    oasp_xml_node *node = context;
    if (node->owns_node && node->node)
        xmlFreeNode(node->node);
    node->node = NULL;
    node->owns_node = false;
}

static void xml_invalidate_nodes(oasp_xml_document *document) {
    document->generation++;
    if (!document->generation)
        document->generation = 1;
}

static bool xml_node_is_live(const oasp_xml_node *node) {
    return node && node->node && node->owner && node->generation == node->owner->generation;
}

static xmlNodePtr xml_context_node(oasp_object *object, oasp_xml_document **owner) {
    if (object->component_kind == OASP_COMPONENT_XML) {
        *owner = object->native;
        return (*owner)->document ? xmlDocGetRootElement((*owner)->document) : NULL;
    }
    if (object->component_kind == OASP_COMPONENT_XML_NODE) {
        oasp_xml_node *node = object->native;
        *owner = node->owner;
        return xml_node_is_live(node) ? node->node : NULL;
    }
    return NULL;
}

static oasp_value xml_node_value_with_ownership(oasp_runtime *runtime, oasp_xml_document *owner, xmlNodePtr node, bool owns_node) {
    if (!node)
        return oasp_value_nothing();
    oasp_xml_node *native = oasp_runtime_alloc(runtime, sizeof(*native), _Alignof(oasp_xml_node));
    if (!native)
        return oasp_value_empty();
    native->owner = owner;
    native->node = node;
    native->generation = owner ? owner->generation : 0;
    native->owns_node = owns_node;
    if (!oasp_runtime_defer(runtime, native, xml_node_finalize, native)) {
        if (owns_node)
            xmlFreeNode(node);
        return oasp_value_empty();
    }
    oasp_object *object = oasp_component_object(runtime, OASP_COMPONENT_XML_NODE, "MSXML2.IXMLDOMNode", native);
    return object ? oasp_component_object_value(object) : oasp_value_empty();
}

static oasp_value xml_node_value(oasp_runtime *runtime, oasp_xml_document *owner, xmlNodePtr node) {
    return xml_node_value_with_ownership(runtime, owner, node, false);
}

static oasp_value xml_attributes(oasp_runtime *runtime, oasp_xml_node *node) {
    oasp_xml_attribute_map *attributes = oasp_runtime_alloc(runtime, sizeof(*attributes), _Alignof(oasp_xml_attribute_map));
    if (!attributes)
        return oasp_value_empty();
    attributes->node = node;
    oasp_object *object = oasp_component_object(runtime, OASP_COMPONENT_XML_ATTRIBUTE_MAP,
                                                "MSXML2.IXMLDOMNamedNodeMap", attributes);
    return object ? oasp_component_object_value(object) : oasp_value_empty();
}

static oasp_value xml_children(oasp_runtime *runtime, oasp_object *object) {
    oasp_xml_document *owner = NULL;
    xmlNodePtr parent = xml_context_node(object, &owner);
    xmlNodePtr child = object->component_kind == OASP_COMPONENT_XML ? owner->document->children : parent ? parent->children : NULL;
    size_t count = 0;
    for (xmlNodePtr current = child; current; current = current->next)
        count++;
    oasp_component_collection *collection = oasp_runtime_alloc(runtime, sizeof(*collection), _Alignof(oasp_component_collection));
    if (!collection)
        return oasp_value_empty();
    collection->count = count;
    collection->items = count ? oasp_runtime_alloc_values(runtime, count) : NULL;
    if (count && !collection->items)
        return oasp_value_empty();
    size_t index = 0;
    for (xmlNodePtr current = child; current; current = current->next)
        collection->items[index++] = xml_node_value(runtime, owner, current);
    oasp_object *list = oasp_component_object(runtime, OASP_COMPONENT_XML_NODE_LIST, "MSXML2.IXMLDOMNodeList", collection);
    return list ? oasp_component_object_value(list) : oasp_value_empty();
}

static oasp_value xml_select(oasp_runtime *runtime, oasp_object *object, const oasp_value *path_value, bool multiple) {
    oasp_xml_document *owner = NULL;
    xmlNodePtr node = xml_context_node(object, &owner);
    char *path = oasp_value_format(runtime->arena, path_value);
    if (!owner || !owner->document || !path)
        return multiple ? oasp_value_empty() : oasp_value_nothing();
    if (!multiple && !strpbrk(path, "/.[@*")) {
        xmlNodePtr child = object->component_kind == OASP_COMPONENT_XML ? owner->document->children : node ? node->children : NULL;
        for (; child; child = child->next)
            if (child->name && !strcmp((const char *)child->name, path))
                return xml_node_value(runtime, owner, child);
        return oasp_value_nothing();
    }
    xmlXPathContextPtr context = xmlXPathNewContext(owner->document);
    if (!context)
        return oasp_value_empty();
    context->node = object->component_kind == OASP_COMPONENT_XML ? (xmlNodePtr)owner->document : node;
    xmlXPathObjectPtr selection = xmlXPathEvalExpression((const xmlChar *)path, context);
    xmlXPathFreeContext(context);
    xmlNodeSetPtr nodes = selection ? selection->nodesetval : NULL;
    if (!multiple) {
        xmlNodePtr first = nodes && nodes->nodeNr > 0 ? nodes->nodeTab[0] : NULL;
        oasp_value result = xml_node_value(runtime, owner, first);
        if (selection)
            xmlXPathFreeObject(selection);
        return result;
    }
    oasp_component_collection *collection = oasp_runtime_alloc(runtime, sizeof(*collection), _Alignof(oasp_component_collection));
    if (!collection) {
        if (selection)
            xmlXPathFreeObject(selection);
        return oasp_value_empty();
    }
    collection->count = nodes && nodes->nodeNr > 0 ? (size_t)nodes->nodeNr : 0;
    collection->items = collection->count ? oasp_runtime_alloc_values(runtime, collection->count) : NULL;
    if (collection->count && !collection->items) {
        if (selection)
            xmlXPathFreeObject(selection);
        return oasp_value_empty();
    }
    for (size_t i = 0; i < collection->count; i++)
        collection->items[i] = xml_node_value(runtime, owner, nodes->nodeTab[i]);
    if (selection)
        xmlXPathFreeObject(selection);
    oasp_object *list = oasp_component_object(runtime, OASP_COMPONENT_XML_NODE_LIST, "MSXML2.IXMLDOMNodeList", collection);
    return list ? oasp_component_object_value(list) : oasp_value_empty();
}

static oasp_value xml_serialized(oasp_runtime *runtime, oasp_xml_document *owner, xmlNodePtr node) {
    if (!owner || !owner->document)
        return oasp_value_string("", 0);
    xmlBufferPtr buffer = xmlBufferCreate();
    if (!buffer)
        return oasp_value_empty();
    if (node)
        (void)xmlNodeDump(buffer, owner->document, node, 0, 0);
    else
        for (xmlNodePtr child = owner->document->children; child; child = child->next)
            (void)xmlNodeDump(buffer, owner->document, child, 0, 0);
    oasp_value result = xml_slice_value(runtime, (const char *)xmlBufferContent(buffer), (size_t)xmlBufferLength(buffer));
    xmlBufferFree(buffer);
    return result;
}

void *oasp_builtin_xml_create(oasp_runtime *runtime) {
    oasp_xml_document *document = oasp_runtime_alloc(runtime, sizeof(*document), _Alignof(oasp_xml_document));
    if (!document)
        return NULL;
    memset(document, 0, sizeof(*document));
    document->generation = 1;
    document->document = xmlNewDoc((const xmlChar *)"1.0");
    if (!document->document || !oasp_runtime_defer(runtime, document, xml_finalize, document)) {
        if (document->document)
            xmlFreeDoc(document->document);
        return NULL;
    }
    return document;
}

bool oasp_builtin_xml_get(oasp_runtime *runtime, oasp_object *object, oasp_slice member, size_t line, oasp_value *result) {
    if (object->component_kind == OASP_COMPONENT_XML) {
        oasp_xml_document *document = object->native;
        if (oasp_ascii_equal(member, "readystate"))
            *result = oasp_value_int(document->document ? 4 : 0);
        else if (oasp_ascii_equal(member, "parseerror")) {
            oasp_object *error = oasp_component_object(runtime, OASP_COMPONENT_XML_PARSE_ERROR, "MSXML2.IXMLDOMParseError", document);
            *result = error ? oasp_component_object_value(error) : oasp_value_empty();
        } else if (oasp_ascii_equal(member, "documentelement"))
            *result = xml_node_value(runtime, document, document->document ? xmlDocGetRootElement(document->document) : NULL);
        else if (oasp_ascii_equal(member, "xml"))
            *result = xml_serialized(runtime, document, NULL);
        else if (oasp_ascii_equal(member, "childnodes"))
            *result = xml_children(runtime, object);
        else
            return false;
        return true;
    }
    if (object->component_kind == OASP_COMPONENT_XML_NODE) {
        oasp_xml_node *node = object->native;
        if (!xml_node_is_live(node)) {
            oasp_runtime_raise(runtime, 91, "XML node is no longer valid", object->component_name, line);
            *result = oasp_value_empty();
        } else if (oasp_ascii_equal(member, "childnodes"))
            *result = xml_children(runtime, object);
        else if (oasp_ascii_equal(member, "nodename"))
            *result = xml_slice_value(runtime, (const char *)node->node->name, strlen((const char *)node->node->name));
        else if (oasp_ascii_equal(member, "text") || oasp_ascii_equal(member, "nodevalue") || oasp_ascii_equal(member, "value")) {
            xmlChar *content = xmlNodeGetContent(node->node);
            *result = xml_slice_value(runtime, content ? (const char *)content : "", content ? strlen((const char *)content) : 0);
            if (content)
                xmlFree(content);
        } else if (oasp_ascii_equal(member, "xml"))
            *result = xml_serialized(runtime, node->owner, node->node);
        else if (oasp_ascii_equal(member, "attributes"))
            *result = xml_attributes(runtime, node);
        else
            return false;
        return true;
    }
    if (object->component_kind == OASP_COMPONENT_XML_NODE_LIST) {
        oasp_component_collection *collection = object->native;
        if (!oasp_ascii_equal(member, "length") && !oasp_ascii_equal(member, "count"))
            return false;
        *result = oasp_value_int((int64_t)collection->count);
        return true;
    }
    if (object->component_kind == OASP_COMPONENT_XML_PARSE_ERROR) {
        oasp_xml_document *document = object->native;
        if (oasp_ascii_equal(member, "errorcode"))
            *result = oasp_value_int(document->error_code);
        else if (oasp_ascii_equal(member, "line"))
            *result = oasp_value_int(document->error_line);
        else if (oasp_ascii_equal(member, "linepos"))
            *result = oasp_value_int(document->error_column);
        else if (oasp_ascii_equal(member, "reason") || oasp_ascii_equal(member, "srctext"))
            *result = oasp_value_string(document->error_reason.data, document->error_reason.length);
        else
            return false;
        return true;
    }
    return false;
}

bool oasp_builtin_xml_set(oasp_runtime *runtime, oasp_object *object, oasp_slice member, const oasp_value *value, size_t line) {
    if (object->component_kind == OASP_COMPONENT_XML_NODE && (oasp_ascii_equal(member, "nodevalue") || oasp_ascii_equal(member, "text"))) {
        oasp_xml_node *node = object->native;
        if (!xml_node_is_live(node))
            oasp_runtime_raise(runtime, 91, "XML node is no longer valid", object->component_name, line);
        else {
            char *text = oasp_value_format(runtime->arena, value);
            if (text)
                xmlNodeSetContent(node->node, (const xmlChar *)text);
        }
        return true;
    }
    return object->component_kind == OASP_COMPONENT_XML &&
           (oasp_ascii_equal(member, "async") || oasp_ascii_equal(member, "validateonparse"));
}

static void xml_replace_document(oasp_xml_document *document, xmlDocPtr parsed) {
    xml_invalidate_nodes(document);
    if (document->document)
        xmlFreeDoc(document->document);
    document->document = parsed;
    document->error_code = 0;
    document->error_line = 0;
    document->error_column = 0;
    document->error_reason = (oasp_slice){"", 0};
}

static bool xml_document_call(oasp_runtime *runtime, oasp_object *object, oasp_slice member, oasp_value *arguments, size_t argument_count,
                              size_t line, oasp_value *result) {
    if (object->component_kind != OASP_COMPONENT_XML)
        return false;
    oasp_xml_document *document = object->native;
    if (oasp_ascii_equal(member, "setproperty")) {
        *result = oasp_value_empty();
        return true;
    }
    if (oasp_ascii_equal(member, "createprocessinginstruction") && argument_count >= 2) {
        char *target = oasp_value_format(runtime->arena, &arguments[0]);
        char *content = oasp_value_format(runtime->arena, &arguments[1]);
        *result = xml_node_value_with_ownership(runtime, document,
                                                 target && content ? xmlNewPI((const xmlChar *)target, (const xmlChar *)content) : NULL, true);
        return true;
    }
    if (oasp_ascii_equal(member, "loadxml") || oasp_ascii_equal(member, "load")) {
        char *text = argument_count ? oasp_value_format(runtime->arena, &arguments[0]) : NULL;
        xmlResetLastError();
        xmlDocPtr parsed = oasp_ascii_equal(member, "loadxml")
                               ? (text ? xmlReadMemory(text, (int)strlen(text), "memory.xml", NULL,
                                                        XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING) : NULL)
                               : (text ? xmlReadFile(oasp_builtin_case_path(runtime, text), NULL,
                                                     XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING) : NULL);
        if (parsed)
            xml_replace_document(document, parsed);
        else {
            const xmlError *error = xmlGetLastError();
            document->error_code = error ? error->code : 1;
            document->error_line = error ? error->line : 0;
            document->error_column = error ? error->int2 : 0;
            if (oasp_ascii_equal(member, "loadxml")) {
                const char *message = error && error->message ? error->message : "XML parse error";
                size_t length = strlen(message);
                while (length && (message[length - 1] == '\r' || message[length - 1] == '\n'))
                    length--;
                document->error_reason.data = oasp_runtime_copy(runtime, message, length);
                document->error_reason.length = length;
            }
        }
        *result = oasp_value_bool(parsed != NULL);
        return true;
    }
    if (oasp_ascii_equal(member, "save")) {
        char *path = argument_count ? oasp_value_format(runtime->arena, &arguments[0]) : NULL;
        char *host = path ? oasp_builtin_host_path(runtime, path) : NULL;
        xmlSaveCtxtPtr save = host && document->document ? xmlSaveToFilename(host, "UTF-8", XML_SAVE_NO_DECL) : NULL;
        int saved = save ? xmlSaveDoc(save, document->document) : -1;
        int closed = save ? xmlSaveClose(save) : -1;
        if (saved < 0 || closed < 0)
            oasp_runtime_raise(runtime, 70, "cannot save XML document", object->component_name, line);
        *result = oasp_value_empty();
        return true;
    }
    if (oasp_ascii_equal(member, "createelement") && argument_count) {
        char *name = oasp_value_format(runtime->arena, &arguments[0]);
        *result = xml_node_value_with_ownership(runtime, document, name ? xmlNewNode(NULL, (const xmlChar *)name) : NULL, true);
        return true;
    }
    if (oasp_ascii_equal(member, "createnode") && argument_count) {
        int64_t type = 0;
        char *name = argument_count > 1 ? oasp_value_format(runtime->arena, &arguments[1]) : NULL;
        if (!oasp_value_to_int(&arguments[0], &type)) {
            char *type_name = oasp_value_format(runtime->arena, &arguments[0]);
            oasp_slice type_slice = {type_name, type_name ? strlen(type_name) : 0};
            if (type_name && oasp_ascii_equal(type_slice, "element"))
                type = XML_ELEMENT_NODE;
            else if (type_name && oasp_ascii_equal(type_slice, "attribute"))
                type = XML_ATTRIBUTE_NODE;
            else if (type_name && oasp_ascii_equal(type_slice, "cdatasection"))
                type = XML_CDATA_SECTION_NODE;
        }
        xmlNodePtr node = type == XML_ELEMENT_NODE && name ? xmlNewNode(NULL, (const xmlChar *)name)
                         : type == XML_ATTRIBUTE_NODE && name ? (xmlNodePtr)xmlNewDocProp(document->document, (const xmlChar *)name, NULL)
                         : type == XML_CDATA_SECTION_NODE ? xmlNewCDataBlock(document->document, (const xmlChar *)"", 0) : NULL;
        *result = xml_node_value_with_ownership(runtime, document, node, true);
        return true;
    }
    return false;
}

bool oasp_builtin_xml_call(oasp_runtime *runtime, oasp_object *object, oasp_slice member, oasp_value *arguments, size_t argument_count,
                           size_t line, oasp_value *result) {
    if (xml_document_call(runtime, object, member, arguments, argument_count, line, result))
        return true;
    if (object->component_kind == OASP_COMPONENT_XML_ATTRIBUTE_MAP && oasp_ascii_equal(member, "setnameditem") && argument_count &&
        arguments[0].kind == OASP_VALUE_OBJECT && arguments[0].as.object->component_kind == OASP_COMPONENT_XML_NODE) {
        oasp_xml_attribute_map *attributes = object->native;
        oasp_xml_node *parent = attributes ? attributes->node : NULL;
        oasp_xml_node *attribute = arguments[0].as.object->native;
        if (!xml_node_is_live(parent) || !xml_node_is_live(attribute) || attribute->node->type != XML_ATTRIBUTE_NODE)
            oasp_runtime_raise(runtime, 91, "XML attribute node is no longer valid", object->component_name, line);
        else {
            xmlChar *content = xmlNodeGetContent(attribute->node);
            xmlAttrPtr attached = xmlSetProp(parent->node, attribute->node->name, content);
            if (content)
                xmlFree(content);
            *result = xml_node_value(runtime, parent->owner, (xmlNodePtr)attached);
            return true;
        }
        *result = oasp_value_empty();
        return true;
    }
    if ((object->component_kind == OASP_COMPONENT_XML || object->component_kind == OASP_COMPONENT_XML_NODE) &&
        oasp_ascii_equal(member, "childnodes")) {
        oasp_value children = xml_children(runtime, object);
        oasp_component_collection *collection = children.kind == OASP_VALUE_OBJECT ? children.as.object->native : NULL;
        int64_t index = -1;
        if (!argument_count)
            *result = children;
        else if (!collection || !oasp_value_to_int(&arguments[0], &index) || index < 0 || (size_t)index >= collection->count)
            *result = oasp_value_nothing();
        else
            *result = collection->items[index];
        return true;
    }
    if ((object->component_kind == OASP_COMPONENT_XML || object->component_kind == OASP_COMPONENT_XML_NODE) &&
        oasp_ascii_equal(member, "insertbefore") && argument_count && arguments[0].kind == OASP_VALUE_OBJECT &&
        arguments[0].as.object->component_kind == OASP_COMPONENT_XML_NODE) {
        oasp_xml_document *owner = NULL;
        xmlNodePtr parent = xml_context_node(object, &owner);
        oasp_xml_node *child = arguments[0].as.object->native;
        if (!owner || !owner->document || (object->component_kind == OASP_COMPONENT_XML_NODE && !parent) ||
            !xml_node_is_live(child)) {
            oasp_runtime_raise(runtime, 91, "XML node is no longer valid", object->component_name, line);
            *result = oasp_value_empty();
            return true;
        }
        oasp_xml_node *reference_node = argument_count > 1 && arguments[1].kind == OASP_VALUE_OBJECT &&
                                                arguments[1].as.object->component_kind == OASP_COMPONENT_XML_NODE
                                            ? arguments[1].as.object->native : NULL;
        xmlNodePtr reference = xml_node_is_live(reference_node) ? reference_node->node : NULL;
        xmlNodePtr inserted = reference ? xmlAddPrevSibling(reference, child->node)
                                        : xmlAddChild(object->component_kind == OASP_COMPONENT_XML ? (xmlNodePtr)owner->document : parent, child->node);
        if (inserted) {
            child->node = inserted;
            child->owner = owner;
            child->generation = owner->generation;
            child->owns_node = false;
        }
        *result = xml_node_value(runtime, owner, inserted);
        return true;
    }
    if ((object->component_kind == OASP_COMPONENT_XML || object->component_kind == OASP_COMPONENT_XML_NODE) && argument_count &&
        (oasp_ascii_equal(member, "selectsinglenode") || oasp_ascii_equal(member, "selectnodes"))) {
        *result = xml_select(runtime, object, &arguments[0], oasp_ascii_equal(member, "selectnodes"));
        return true;
    }
    if (object->component_kind == OASP_COMPONENT_XML && oasp_ascii_equal(member, "appendchild") && argument_count &&
        arguments[0].kind == OASP_VALUE_OBJECT && arguments[0].as.object->component_kind == OASP_COMPONENT_XML_NODE) {
        oasp_xml_document *document = object->native;
        oasp_xml_node *child = arguments[0].as.object->native;
        if (!xml_node_is_live(child)) {
            oasp_runtime_raise(runtime, 91, "XML child node is no longer valid", object->component_name, line);
            *result = oasp_value_empty();
            return true;
        }
        xmlNodePtr old_root = xmlDocSetRootElement(document->document, child->node);
        if (old_root && old_root != child->node) {
            xml_invalidate_nodes(document);
            xmlFreeNode(old_root);
        }
        child->owner = document;
        child->generation = document->generation;
        child->owns_node = false;
        *result = arguments[0];
        return true;
    }
    if (object->component_kind != OASP_COMPONENT_XML_NODE)
        return false;
    oasp_xml_node *node = object->native;
    if (!xml_node_is_live(node)) {
        oasp_runtime_raise(runtime, 91, "XML node is no longer valid", object->component_name, line);
        *result = oasp_value_empty();
        return true;
    }
    if (oasp_ascii_equal(member, "appendchild") && argument_count && arguments[0].kind == OASP_VALUE_OBJECT &&
        arguments[0].as.object->component_kind == OASP_COMPONENT_XML_NODE) {
        oasp_xml_node *child = arguments[0].as.object->native;
        if (!xml_node_is_live(child))
            oasp_runtime_raise(runtime, 91, "XML child node is no longer valid", object->component_name, line);
        else {
            xmlNodePtr appended = xmlAddChild(node->node, child->node);
            if (appended) {
                child->node = appended;
                child->owner = node->owner;
                child->generation = node->owner->generation;
                child->owns_node = false;
            }
            *result = xml_node_value(runtime, node->owner, appended);
            return true;
        }
        *result = oasp_value_empty();
        return true;
    }
    if (oasp_ascii_equal(member, "getattributenode") && argument_count) {
        char *name = oasp_value_format(runtime->arena, &arguments[0]);
        *result = xml_node_value(runtime, node->owner, name ? (xmlNodePtr)xmlHasProp(node->node, (const xmlChar *)name) : NULL);
        return true;
    }
    if (oasp_ascii_equal(member, "setattribute") && argument_count >= 2) {
        char *name = oasp_value_format(runtime->arena, &arguments[0]);
        char *value = oasp_value_format(runtime->arena, &arguments[1]);
        if (name && value)
            (void)xmlSetProp(node->node, (const xmlChar *)name, (const xmlChar *)value);
        *result = oasp_value_empty();
        return true;
    }
    return false;
}

bool oasp_component_xml_snapshot(const oasp_object *object, char **data, size_t *length) {
    if (!object || object->component_kind != OASP_COMPONENT_XML || !data || !length)
        return false;
    oasp_xml_document *document = object->native;
    xmlChar *serialized = NULL;
    int serialized_length = 0;
    if (!document || !document->document)
        return false;
    xmlDocDumpMemoryEnc(document->document, &serialized, &serialized_length, "UTF-8");
    if (!serialized || serialized_length < 0) {
        if (serialized)
            xmlFree(serialized);
        return false;
    }
    oasp_buffer escaped;
    if (!oasp_buffer_init(&escaped, (size_t)serialized_length + 1)) {
        xmlFree(serialized);
        return false;
    }
    const char *cursor = (const char *)serialized;
    const char *end = cursor + (size_t)serialized_length;
    while (cursor < end) {
        const char *entity_end = *cursor == '&' ? memchr(cursor, ';', (size_t)(end - cursor)) : NULL;
        bool standard_entity = entity_end && (((size_t)(entity_end - cursor) == 4 && !memcmp(cursor, "&amp;", 5)) ||
            ((size_t)(entity_end - cursor) == 3 && (!memcmp(cursor, "&lt;", 4) || !memcmp(cursor, "&gt;", 4))) ||
            ((size_t)(entity_end - cursor) == 5 && !memcmp(cursor, "&quot;", 6)) ||
            ((size_t)(entity_end - cursor) == 4 && !memcmp(cursor, "&apos;", 5)) ||
            (entity_end - cursor > 2 && cursor[1] == '#'));
        if (entity_end && !standard_entity) {
            if (!oasp_buffer_append(&escaped, "&amp;", 5))
                break;
            cursor++;
        } else if (!oasp_buffer_append_byte(&escaped, *cursor++))
            break;
    }
    xmlFree(serialized);
    if (cursor != end) {
        oasp_buffer_dispose(&escaped);
        return false;
    }
    *data = escaped.data;
    *length = escaped.length;
    return true;
}

bool oasp_component_xml_restore(oasp_runtime *runtime, oasp_slice data, oasp_object **result) {
    if (!runtime || !result || data.length > INT_MAX)
        return false;
    oasp_value created = oasp_component_create(runtime, (oasp_slice){"MSXML2.DOMDocument", 18}, 0);
    if (created.kind != OASP_VALUE_OBJECT || !created.as.object)
        return false;
    xmlDocPtr parsed = xmlReadMemory(data.data, (int)data.length, "state.xml", NULL,
                                     XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
    if (!parsed)
        return false;
    xml_replace_document(created.as.object->native, parsed);
    *result = created.as.object;
    return true;
}
