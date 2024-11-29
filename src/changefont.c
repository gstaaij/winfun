#define NOB_IMPLEMENTATION
#define NOB_STRIP_PREFIX
#include "nob.h"
// Undefine the log error types, because it conflicts with windows.h
#undef ERROR
#undef INFO
#undef WARNING

#include "winver.h"

#include <malloc.h>
#include <string.h>
#include <windows.h>

#define MAX_KEY_LENGTH 255
#define MAX_VALUE_NAME 16383
#define MAX_VALUE_DATA 16383

void sb_append_escaped(String_Builder* sb, const char* string) {
    size_t string_len = strlen(string);
    for (size_t i = 0; i < string_len; ++i) {
        char chr = string[i];
        switch (chr) {
        case '\\':
            sb_append_cstr(sb, "\\\\");
            break;
        case '\n':
            sb_append_cstr(sb, "\\n");
            break;
        default:
            da_append(sb, chr);
            break;
        }
    }
}

typedef enum {
    REG_TYPE_STRING,
    REG_TYPE_HEX,
    REG_TYPE_DELETE,
} Registry_Value_Type;

typedef struct {
    char* name;
    size_t name_len;
    Registry_Value_Type type;
    DWORD type_hex_type;
    char* data;
    size_t data_len;
} Registry_Value;

typedef struct {
    Registry_Value* items;
    size_t count;
    size_t capacity;
} Registry_Value_List;

bool reg_key_list_values(HKEY parent_key, Registry_Value_List* result) {
    char value_name[MAX_VALUE_NAME];
    unsigned char value_data[MAX_VALUE_DATA+1];
    DWORD amount_of_values = 0;

    int code = RegQueryInfoKeyA(parent_key, NULL, NULL, NULL, NULL /*Amount of subkeys*/, NULL, NULL, &amount_of_values, NULL, NULL, NULL, NULL);
    if (code != ERROR_SUCCESS) {
        nob_log(NOB_ERROR, "Couldn't query registry key info: %ld", GetLastError());
        return false;
    }

    for (DWORD i = 0; i < amount_of_values; ++i) {
        Registry_Value key = {0};

        DWORD value_len = MAX_VALUE_NAME;
        DWORD value_type;
        DWORD data_len = MAX_VALUE_DATA;
        code = RegEnumValueA(parent_key, i, value_name, &value_len, NULL, &value_type, value_data, &data_len);
        if (code != ERROR_SUCCESS) {
            nob_log(NOB_ERROR, "Couldn't enumerate value %ld of %ld: %ld", i, amount_of_values, code);
            return false;
        }
        key.name_len = value_len;
        key.name = malloc(sizeof(*value_name) * (value_len + 1));
        memcpy(key.name, value_name, sizeof(*value_name) * value_len);
        // Ensure the name is null-terminated
        key.name[value_len] = 0;

        key.data_len = data_len;
        key.data = malloc(sizeof(*value_data) * (data_len + 1));
        memcpy(key.data, value_data, sizeof(*value_data) * data_len);
        // Ensure the name is null-terminated
        key.data[data_len] = 0;
        if (value_type == REG_SZ) {
            key.type = REG_TYPE_STRING;
        } else {
            key.type = REG_TYPE_HEX;
            key.type_hex_type = value_type;
        }

        da_append(result, key);
    }

    return true;
}

void reg_sb_append_hex(String_Builder* sb, const Registry_Value* value) {
    sb_append_cstr(sb, temp_sprintf("hex(%ld):", value->type_hex_type));
    for (size_t i = 0; i < value->data_len; ++i) {
        if (i > 0)
            da_append(sb, ',');
        sb_append_cstr(sb, temp_sprintf("%x", value->data[i]));
    }
}

bool reg_key_add_to_file(const char* registry_path, const Registry_Value_List list, String_Builder* sb) {
    sb_append_cstr(sb, "\n");
    sb_append_cstr(sb, temp_sprintf("[HKEY_LOCAL_MACHINE\\%s]\n", registry_path));
    for (size_t i = 0; i < list.count; ++i) {
        sb_append_cstr(sb, "\"");
        sb_append_escaped(sb, list.items[i].name);
        sb_append_cstr(sb, "\"=");
        switch (list.items[i].type) {
        case REG_TYPE_STRING:
            sb_append_cstr(sb, "\"");
            sb_append_escaped(sb, list.items[i].data);
            sb_append_cstr(sb, "\"");
            break;
        case REG_TYPE_HEX:
            reg_sb_append_hex(sb, &list.items[i]);
            break;
        case REG_TYPE_DELETE:
            da_append(sb, '-');
            break;
        }
        sb_append_cstr(sb, "\n");
    }
    return true;
}

bool reg_key_get_file(const char* registry_path, const Registry_Value_List list, String_Builder* sb) {
    sb->count = 0;
    sb_append_cstr(sb, "Windows Registry Editor Version 5.00\n");
    return reg_key_add_to_file(registry_path, list, sb);
}

bool util_is_admin() {
    DWORD cbSid = SECURITY_MAX_SID_SIZE;
    PSID pSid = _alloca(cbSid);
    BOOL isAdmin;

    if (!CreateWellKnownSid(WinBuiltinAdministratorsSid, NULL, pSid, &cbSid)) {
        nob_log(NOB_ERROR, "CreateWellKnownSid failed with error %d", GetLastError());
        exit(1);
    }

    if (!CheckTokenMembership(NULL, pSid, &isAdmin)) {
        nob_log(NOB_ERROR, "CheckTokenMembership failed with error %d", GetLastError());
        exit(1);
    }

    return isAdmin;
}

bool str_contains(const char* haystack, const char* needle) {
    size_t hay_len = strlen(haystack);
    size_t needle_len = strlen(needle);
    if (needle_len > hay_len) return false;
    if (memcmp(haystack, needle, needle_len) == 0) return true;

    size_t count = 0;
    for (size_t i = 0; i < hay_len; ++i) {
        if (i + needle_len - count > hay_len) return false;

        if (tolower(haystack[i]) == tolower(needle[count])) {
            count++;
            if (count == needle_len) return true;
        } else
            count = 0;
    }
    return false;
}

#define FONTS_REGISTRY_PATH "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Fonts"
#define FONT_SUBSTITUTES_REGISTRY_PATH "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\FontSubstitutes"
#define FONT_LINK_REGISTRY_PATH "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\FontLink\\SystemLink"

#define BACKUP_FONTS_REG_FILENAME "backup_fonts.reg"
// #define BACKUP_FONTLINK_REG_FILENAME "backup_fontlink.reg"

int main() {
    int result = 0;
    HKEY fonts_key = 0;
    HKEY font_substitutes_key = 0;
    HKEY font_link_key = 0;

    // if (!util_is_admin()) {
    //     nob_log(NOB_ERROR, "You need to run this tool with Administrator privileges!");
    //     return_defer(10);
    // }

    char exe_dir[MAX_PATH];
    DWORD exe_dir_len = GetModuleFileNameA(NULL, exe_dir, MAX_PATH);
    DWORD error = GetLastError();
    if (error != ERROR_SUCCESS) {
        nob_log(NOB_ERROR, "Couldn't get module file name: %ld", error);
        return_defer(1);
    }
    for (int i = exe_dir_len - 1; i >= 0; --i) {
        if (exe_dir[i] == '\\') {
            exe_dir[i] = '\0';
            break;
        }
    }

    long code = RegOpenKeyExA(HKEY_LOCAL_MACHINE, FONTS_REGISTRY_PATH, 0, KEY_READ, &fonts_key);
    if (code != ERROR_SUCCESS) {
        nob_log(NOB_ERROR, "Failed to open key %s: %ld", FONTS_REGISTRY_PATH, code);
        return_defer(40);
    }
    Registry_Value_List font_list = {0};
    if (!reg_key_list_values(fonts_key, &font_list)) return_defer(1);
    nob_log(NOB_INFO, "Amount of fonts: %zu", font_list.count);

    code = RegOpenKeyExA(HKEY_LOCAL_MACHINE, FONT_LINK_REGISTRY_PATH, 0, KEY_READ, &font_link_key);
    if (code != ERROR_SUCCESS) {
        nob_log(NOB_ERROR, "Failed to open key %s: %ld", FONT_LINK_REGISTRY_PATH, code);
        return_defer(41);
    }
    Registry_Value_List font_link_list = {0};
    if (!reg_key_list_values(font_link_key, &font_link_list)) return_defer(1);
    nob_log(NOB_INFO, "Amount of font links: %zu", font_link_list.count);

    printf("Now, you will choose a font to replace all other fonts with.\n");
    printf("The amount of fonts is probably too high to list them now.\nThat's why you can search through them.\n");
retry_search_query:
    printf("Search query: ");
    #define QUERY_MAX_LEN 128
    char query[QUERY_MAX_LEN] = {0};
    fgets(query, QUERY_MAX_LEN, stdin);
    size_t query_len = strlen(query);
    if (query[query_len - 1] == '\n')
        query[query_len - 1] = 0;

    printf("Fonts that match the query:\n");
    bool found_font = false;
    for (size_t i = 0; i < font_list.count; ++i) {
        if (str_contains(font_list.items[i].name, query)) {
            printf("  [%zu] %s\n", i, font_list.items[i].name);
            found_font = true;
        }
    }
    if (!found_font) {
        nob_log(NOB_ERROR, "No fonts were found, try again.");
        goto retry_search_query;
    }
    
retry_number_query:
    memset(query, 0, sizeof(*query) * QUERY_MAX_LEN);
    printf("Enter the number of the font you want: ");
    fgets(query, QUERY_MAX_LEN, stdin);
    query_len = strlen(query);
    if (query[query_len - 1] == '\n')
        query[query_len - 1] = 0;
    int font_index = atoi(query);
    if (font_index < 0 || font_index >= (int) font_list.count) {
        nob_log(NOB_ERROR, "Invalid number, try again.");
        goto retry_number_query;
    }
    
    printf("\n");
    printf("This will create a .reg file to replace ALL fonts with `%s`.\n", font_list.items[font_index].name);
    printf("A backup .reg file will be created and can be restored later.\n");
    memset(query, 0, sizeof(*query) * QUERY_MAX_LEN);
    printf("Are you SURE you want to continue? [y/N] ");
    fgets(query, QUERY_MAX_LEN, stdin);

    if (tolower(query[0]) != 'y') return 0;

    Registry_Value_List font_substitute_list = {0};

    for (size_t i = 0; i < font_list.count; ++i) {
        Registry_Value val = {
            .name = malloc(sizeof(char) * font_list.items[i].name_len),
            .name_len = font_list.items[i].name_len,
            .data = NULL,
            .data_len = 0,
            .type = REG_TYPE_DELETE,
        };

        memcpy(val.name, font_list.items[i].name, sizeof(char) * font_list.items[i].name_len);
        while (val.name[val.name_len - 1] == '\0') --val.name_len;
        
        if (val.name[val.name_len - 1] == ')') {
            while (val.name_len > 0 && val.name[val.name_len - 1] != '(')
                --val.name_len;
            --val.name_len;
            if (val.name_len > 0 && val.name[val.name_len - 1] == ' ')
                --val.name_len;
        }
        val.name[val.name_len] = '\0';

        da_append(&font_substitute_list, val);
    }

    code = RegOpenKeyExA(HKEY_LOCAL_MACHINE, FONT_SUBSTITUTES_REGISTRY_PATH, 0, KEY_READ, &font_substitutes_key);
    if (code != ERROR_SUCCESS) {
        nob_log(NOB_ERROR, "Failed to open key %s: %ld", FONT_SUBSTITUTES_REGISTRY_PATH, code);
        return_defer(40);
    }
    if (!reg_key_list_values(font_substitutes_key, &font_substitute_list)) return_defer(1);
    nob_log(NOB_INFO, "Amount of font substitutes: %zu", font_substitute_list.count);

    String_Builder font_reg = {0};

    if (file_exists(temp_sprintf("%s/%s", exe_dir, BACKUP_FONTS_REG_FILENAME))
    ) {
        nob_log(NOB_WARNING, "A backup already exists! Not overwriting the file.");
    } else {
        if (!reg_key_get_file(FONTS_REGISTRY_PATH, font_list, &font_reg)) return 1;
        if (!reg_key_add_to_file(FONT_SUBSTITUTES_REGISTRY_PATH, font_substitute_list, &font_reg)) return 1;
        if (!reg_key_add_to_file(FONT_LINK_REGISTRY_PATH, font_link_list, &font_reg)) return 1;
        char* fonts_backup_file_path = temp_sprintf("%s/%s", exe_dir, BACKUP_FONTS_REG_FILENAME);
        if (!write_entire_file(fonts_backup_file_path, font_reg.items, font_reg.count)) return false;
        nob_log(NOB_INFO, "Wrote fonts backup file to %s", fonts_backup_file_path);
        temp_reset();

        // font_reg.count = 0;
        // if (!reg_key_get_file(FONTLINK_REGISTRY_PATH, font_link_list, &font_reg)) return 1;
        // char* backup_file_path = temp_sprintf("%s/%s", exe_dir, BACKUP_FONTLINK_REG_FILENAME);
        // if (!write_entire_file(backup_file_path, font_reg.items, font_reg.count)) return false;
        // nob_log(NOB_INFO, "Wrote font link backup file to %s", backup_file_path);
        // temp_reset();
    }

    for (size_t i = 0; i < font_list.count; ++i) {
        if (i == (size_t) font_index) continue;
        font_list.items[i].data = "";
        font_list.items[i].data_len = 0;
    }
    for (size_t i = 0; i < font_substitute_list.count; ++i) {
        if (i == (size_t) font_index) continue;
        font_substitute_list.items[i].data = font_substitute_list.items[font_index].name;
        font_substitute_list.items[i].data_len = font_substitute_list.items[font_index].name_len;
        font_substitute_list.items[i].type = REG_TYPE_STRING;
    }
    for (size_t i = 0; i < font_link_list.count; ++i) {
        font_link_list.items[i].type = REG_TYPE_DELETE;
    }

    if (!reg_key_get_file(FONTS_REGISTRY_PATH, font_list, &font_reg)) return 1;
    if (!reg_key_add_to_file(FONT_SUBSTITUTES_REGISTRY_PATH, font_substitute_list, &font_reg)) return 1;
    if (!reg_key_add_to_file(FONT_LINK_REGISTRY_PATH, font_link_list, &font_reg)) return 1;
    char* fonts_backup_file_path = temp_sprintf("%s/fonts_%s.reg", exe_dir, font_list.items[font_index].name);
    if (!write_entire_file(fonts_backup_file_path, font_reg.items, font_reg.count)) return false;
    nob_log(NOB_INFO, "Wrote fonts registry file to %s", fonts_backup_file_path);
    temp_reset();

    printf("\n\n");
    printf("You can now import the generated fonts_%s.reg file.\n", font_list.items[font_index].name);
    printf("To restore things to normal, import the "BACKUP_FONTS_REG_FILENAME" file.\n");
    printf("Have fun!\n");
    printf("\n");

defer:
    if (fonts_key) RegCloseKey(fonts_key);
    if (font_substitutes_key) RegCloseKey(font_substitutes_key);
    if (font_link_key) RegCloseKey(font_link_key);
    return result;
}
