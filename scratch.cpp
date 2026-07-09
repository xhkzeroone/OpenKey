#include "openkey_adapter.h"
#include <iostream>

int main() {
    openkey::OpenKeyAdapter adapter;
    adapter.setInputType(0);    // Telex
    adapter.setCodeTable(0);    // Unicode
    adapter.setQuickTelex(true); // <--- Enable QuickTelex

    std::string word;
    std::string ascii = "dduwocj";
    for (char c : ascii) {
        auto r = adapter.processAsciiKey(word, c);
        if (r.handled) {
            word = std::move(r.newWord);
        } else {
            word.push_back(c);
        }
    }
    std::cout << "dduwocj with QuickTelex: " << word << std::endl;
    return 0;
}
