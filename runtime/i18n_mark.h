// LN("..."): English menu text kept in a table or array and shown later, through L() / L_line() or an option row
// (runtime/ui/i18n.h), which looks it up then. The mark changes nothing; tools/menu_strings.py collects these, the
// L() / L_line() literals and the option rows' literals as the text to translate.
#pragma once
#define LN(s) s
