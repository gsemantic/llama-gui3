#pragma once

#include <map>
#include <string>
#include <vector>

namespace news_rewriter {

// Узел мини-DOM. Имя хранится в двух видах:
//   full_name — как в документе (с префиксом namespace, напр. "content:encoded");
//   name      — локальное имя без префикса ("encoded").
struct XmlNode {
    std::string full_name;
    std::string name;
    std::map<std::string, std::string> attrs;
    std::vector<XmlNode> children;
    std::string text;             // текстовое содержимое (после декодирования)
};

// Парсит XML в дерево. Возвращает false при синтаксической ошибке.
bool parse_xml(const std::string& xml, XmlNode& root);

// Хелперы навигации по дереву.
// Первый прямой потомок с указанным локальным именем.
const XmlNode* find_child(const XmlNode& node, const std::string& local_name);
// Все прямые потомки с указанным локальным именем.
std::vector<const XmlNode*> find_children(const XmlNode& node,
                                          const std::string& local_name);
// Текст прямого потомка (пусто, если потомка нет).
std::string child_text(const XmlNode& node, const std::string& local_name);
// Полный текст узла (свой текст + текст всех потомков).
std::string full_text(const XmlNode& node);

} // namespace news_rewriter
