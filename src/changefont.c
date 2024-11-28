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

typedef struct {
    char* name;
    size_t name_len;
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
    unsigned char value_data[MAX_PATH+1];
    DWORD amount_of_values = 0;

    int code = RegQueryInfoKeyA(parent_key, NULL, NULL, NULL, NULL /*Amount of values*/, NULL, NULL, &amount_of_values, NULL, NULL, NULL, NULL);
    if (code != ERROR_SUCCESS) {
        nob_log(NOB_ERROR, "Couldn't query registry key info: %ld", GetLastError());
        return false;
    }

    for (DWORD i = 0; i < amount_of_values; ++i) {
        Registry_Value key = {0};

        DWORD value_len = MAX_VALUE_NAME;
        DWORD value_type;
        DWORD data_len = MAX_PATH;
        code = RegEnumValueA(parent_key, i, value_name, &value_len, NULL, &value_type, value_data, &data_len);
        if (code != ERROR_SUCCESS) return false;
        key.name_len = value_len;
        key.name = malloc(sizeof(*value_name) * (value_len + 1));
        memcpy(key.name, value_name, sizeof(*value_name) * value_len);
        // Ensure the name is null-terminated
        key.name[value_len] = 0;

        if (value_type == REG_SZ) {
            key.data_len = data_len;
            key.data = malloc(sizeof(*value_data) * (data_len + 1));
            memcpy(key.data, value_data, sizeof(*value_data) * data_len);
            // Ensure the name is null-terminated
            key.data[data_len] = 0;
        }

        nob_da_append(result, key);
    }

    return true;
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

int main() {
    int result = 0;

    if (!util_is_admin()) {
        nob_log(NOB_ERROR, "You need to run this tool with Administrator privileges!");
        return_defer(10);
    }

    HKEY fonts_key = 0;
    long code = RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Fonts", 0, KEY_READ, &fonts_key);
    if (code != ERROR_SUCCESS) return_defer(1);

    Registry_Value_List font_list = {0};
    if (!reg_key_list_values(fonts_key, &font_list)) return_defer(1);
    nob_log(NOB_INFO, "Amount of fonts: %zu", font_list.count);

    printf("Now, you will choose a font to replace all other fonts with.\n");
    printf("The amount of fonts is probably too high to list them now.\nThat's why you can search through them.\n");
retry_query:
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
        printf("  No fonts were found, try again.\n");
        goto retry_query;
    }
    
    memset(query, 0, sizeof(*query) * QUERY_MAX_LEN);
    printf("Enter the number of the font you want: ");
    fgets(query, QUERY_MAX_LEN, stdin);
    query_len = strlen(query);
    if (query[query_len - 1] == '\n')
        query[query_len - 1] = 0;
    
    printf("%s\n", query);

    TODO("implement font changing");

defer:
    if (fonts_key) RegCloseKey(fonts_key);
    return result;
}
