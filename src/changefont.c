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

// Escape a string and add it to a string builder
void sb_append_escaped(String_Builder* sb, const char* string) {
    size_t string_len = strlen(string);
    // Loop through all characters in the string
    for (size_t i = 0; i < string_len; ++i) {
        char chr = string[i];
        switch (chr) {
        // If this character is a `\` or `\n`, add an escaped character to the string builder
        case '\\':
            sb_append_cstr(sb, "\\\\");
            break;
        case '\n':
            sb_append_cstr(sb, "\\n");
            break;
        default:
            // Otherwise, add the unmodified character
            da_append(sb, chr);
            break;
        }
    }
}

// Possible types for Registry_Value
typedef enum {
    REG_TYPE_STRING,
    REG_TYPE_HEX,
    REG_TYPE_DELETE,
} Registry_Value_Type;

// Structure that stores a Windows registry value
typedef struct {
    char* name;
    size_t name_len;
    Registry_Value_Type type;
    DWORD type_hex_type;
    char* data;
    size_t data_len;
} Registry_Value;

// List of registry values
typedef struct {
    Registry_Value* items;
    size_t count;
    size_t capacity;
} Registry_Value_List;

// Get all of the values for the HKEY parent_key, and add them to the Registry_Value_List result
// Returns true on success, false on failure
bool reg_key_list_values(HKEY parent_key, Registry_Value_List* result) {
    char value_name[MAX_VALUE_NAME];
    unsigned char value_data[MAX_VALUE_DATA+1];
    DWORD amount_of_values = 0;

    // Query the amount of values
    int code = RegQueryInfoKeyA(parent_key, NULL, NULL, NULL, NULL /*Amount of subkeys*/, NULL, NULL, &amount_of_values, NULL, NULL, NULL, NULL);
    if (code != ERROR_SUCCESS) {
        nob_log(NOB_ERROR, "Couldn't query registry key info: %ld", GetLastError());
        return false;
    }

    // Add all of the values to the list
    for (DWORD i = 0; i < amount_of_values; ++i) {
        Registry_Value key = {0};

        DWORD value_len = MAX_VALUE_NAME;
        DWORD value_type;
        DWORD data_len = MAX_VALUE_DATA;
        // Retrieve the name and data of this value
        code = RegEnumValueA(parent_key, i, value_name, &value_len, NULL, &value_type, value_data, &data_len);
        if (code != ERROR_SUCCESS) {
            nob_log(NOB_ERROR, "Couldn't enumerate value %ld of %ld: %ld", i, amount_of_values, code);
            return false;
        }
        // Copy the name to the registry value
        key.name_len = value_len;
        key.name = malloc(sizeof(*value_name) * (value_len + 1));
        memcpy(key.name, value_name, sizeof(*value_name) * value_len);
        // Ensure the name is null-terminated
        key.name[value_len] = 0;

        // Copy the data to the registry value
        key.data_len = data_len;
        key.data = malloc(sizeof(*value_data) * (data_len + 1));
        memcpy(key.data, value_data, sizeof(*value_data) * data_len);
        // Ensure the data is null-terminated
        key.data[data_len] = 0;

        // Assign the correct type
        if (value_type == REG_SZ) {
            key.type = REG_TYPE_STRING;
        } else {
            key.type = REG_TYPE_HEX;
            key.type_hex_type = value_type;
        }

        // Add the registry value to the list
        da_append(result, key);
    }

    return true;
}

// Add a registry hex value to a string builder
// Returns true on success, false on failure
void reg_sb_append_hex(String_Builder* sb, const Registry_Value* value) {
    assert(value->type == REG_TYPE_HEX);

    sb_append_cstr(sb, temp_sprintf("hex(%ld):", value->type_hex_type));
    for (size_t i = 0; i < value->data_len; ++i) {
        if (i > 0)
            da_append(sb, ',');
        sb_append_cstr(sb, temp_sprintf("%x", value->data[i]));
    }
}

// Add registry values of a key to a string builder in the form of a .reg file
// Doesn't add a header or clear the string builder, to allow for multiple keys per file
// Returns true on success, false on failure
bool reg_key_add_to_file(const char* registry_path, const Registry_Value_List list, String_Builder* sb) {
    sb_append_cstr(sb, "\n");
    sb_append_cstr(sb, temp_sprintf("[HKEY_LOCAL_MACHINE\\%s]\n", registry_path));
    for (size_t i = 0; i < list.count; ++i) {
        // Add the value name
        sb_append_cstr(sb, "\"");
        sb_append_escaped(sb, list.items[i].name);
        sb_append_cstr(sb, "\"=");

        // Add the value data
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

// Add registry values of a key to a string builder in the form of a .reg file
// Resets the string builder and adds the header
// Returns true on success, false on failure
bool reg_key_get_file(const char* registry_path, const Registry_Value_List list, String_Builder* sb) {
    sb->count = 0;
    sb_append_cstr(sb, "Windows Registry Editor Version 5.00\n");
    return reg_key_add_to_file(registry_path, list, sb);
}

// Utility function to determine whether the program is executed with administrative privileges
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

// Check if a string contains another string
// Case insensitive
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

// Print a welcome message
void print_welcome() {
    printf("\n\n");
    printf("Welcome to the winfun font changer!\n");
    printf("\n\n");
    printf("WARNING: USE THIS PROGRAM AT YOUR OWN RISK.\n");
    printf("         ANY DAMAGE DONE TO YOUR COMPUTER IS\n");
    printf("         YOUR RESPONSIBILITY.\n");
    printf("\n");
    printf("Please keep in mind that this program is not yet fully functional.\n");
    printf("Not all fonts will be replaced by your font of choice.\n");
    printf("This will be fixed at some point.\n");
    printf("\n");
}

// Registry paths
#define FONTS_REGISTRY_PATH "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Fonts"
#define FONT_SUBSTITUTES_REGISTRY_PATH "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\FontSubstitutes"
#define FONT_LINK_REGISTRY_PATH "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\FontLink\\SystemLink"

#define BACKUP_FONTS_REG_FILENAME "backup_fonts.reg"

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
    // Get the executable path and path length
    DWORD exe_dir_len = GetModuleFileNameA(NULL, exe_dir, MAX_PATH);
    DWORD error = GetLastError();
    if (error != ERROR_SUCCESS) {
        nob_log(NOB_ERROR, "Couldn't get module file name: %ld", error);
        return_defer(1);
    }
    // Strip off the end to get the directory where the executable is stored
    for (int i = exe_dir_len - 1; i >= 0; --i) {
        if (exe_dir[i] == '\\') {
            exe_dir[i] = '\0';
            break;
        }
    }

    // Open the key for fonts
    long code = RegOpenKeyExA(HKEY_LOCAL_MACHINE, FONTS_REGISTRY_PATH, 0, KEY_READ, &fonts_key);
    if (code != ERROR_SUCCESS) {
        nob_log(NOB_ERROR, "Failed to open key %s: %ld", FONTS_REGISTRY_PATH, code);
        return_defer(40);
    }
    // Get the values of the fonts key
    Registry_Value_List font_list = {0};
    if (!reg_key_list_values(fonts_key, &font_list)) return_defer(1);
    nob_log(NOB_INFO, "Amount of fonts: %zu", font_list.count);

    // Open the font link key
    code = RegOpenKeyExA(HKEY_LOCAL_MACHINE, FONT_LINK_REGISTRY_PATH, 0, KEY_READ, &font_link_key);
    if (code != ERROR_SUCCESS) {
        nob_log(NOB_ERROR, "Failed to open key %s: %ld", FONT_LINK_REGISTRY_PATH, code);
        return_defer(41);
    }
    // Get the values of the font link key
    Registry_Value_List font_link_list = {0};
    if (!reg_key_list_values(font_link_key, &font_link_list)) return_defer(1);
    nob_log(NOB_INFO, "Amount of font links: %zu", font_link_list.count);

    // Print the welcome message
    print_welcome();

    printf("Now, you will choose a font to replace all other fonts with.\n");
    printf("The amount of fonts is probably too high to list them now.\nThat's why you can search through them.\n");
retry_search_query:
    printf("Search query: ");
    #define QUERY_MAX_LEN 128
    char query[QUERY_MAX_LEN] = {0};
    // Get query from standard input and remove trailing newline
    fgets(query, QUERY_MAX_LEN, stdin);
    size_t query_len = strlen(query);
    if (query[query_len - 1] == '\n')
        query[query_len - 1] = 0;

    printf("Fonts that match the query:\n");
    bool found_font = false;
    // List the fonts that match the search query
    for (size_t i = 0; i < font_list.count; ++i) {
        if (str_contains(font_list.items[i].name, query)) {
            printf("  [%zu] %s\n", i, font_list.items[i].name);
            found_font = true;
        }
    }
    // If no fonts are found, ask the user to search again
    if (!found_font) {
        nob_log(NOB_ERROR, "No fonts were found, try again.");
        goto retry_search_query;
    }
    
retry_number_query:
    // Reset the query buffer
    memset(query, 0, sizeof(*query) * QUERY_MAX_LEN);
    printf("Enter the number of the font you want: ");
    // Get query from standard input again and remove trailing newline
    fgets(query, QUERY_MAX_LEN, stdin);
    query_len = strlen(query);
    if (query[query_len - 1] == '\n')
        query[query_len - 1] = 0;
    int font_index = atoi(query);
    // If the number is invalid, prompt the user to try again
    if (font_index < 0 || font_index >= (int) font_list.count) {
        nob_log(NOB_ERROR, "Invalid number, try again.");
        goto retry_number_query;
    }
    
    printf("\n");
    printf("This will create a .reg file to replace ALL fonts with `%s`.\n", font_list.items[font_index].name);
    printf("A backup .reg file will be created and can be restored later.\n");
    memset(query, 0, sizeof(*query) * QUERY_MAX_LEN);
    printf("Do you want to continue? [Y/n] ");
    fgets(query, QUERY_MAX_LEN, stdin);

    // We don't need to strip the trailing newline, because only the first character is checked
    if (tolower(query[0]) == 'n') return 0;

    Registry_Value_List font_substitute_list = {0};

    // Set up font substitute list for the backup
    for (size_t i = 0; i < font_list.count; ++i) {
        Registry_Value val = {
            .name = malloc(sizeof(char) * font_list.items[i].name_len),
            .name_len = font_list.items[i].name_len,
            .data = NULL,
            .data_len = 0,
            // This value needs to be deleted to restore the original state
            .type = REG_TYPE_DELETE,
        };

        // Copy the font item name into this value's name
        memcpy(val.name, font_list.items[i].name, sizeof(char) * font_list.items[i].name_len);
        // Remove trailing zeroes
        while (val.name[val.name_len - 1] == '\0') --val.name_len;
        
        // Remove trailing bracketed information (e.g. ` (TrueType)`) as it is not part of the font name
        if (val.name[val.name_len - 1] == ')') {
            while (val.name_len > 0 && val.name[val.name_len - 1] != '(')
                --val.name_len;
            --val.name_len;
            if (val.name_len > 0 && val.name[val.name_len - 1] == ' ')
                --val.name_len;
        }
        // Add back in a trailing zero
        val.name[val.name_len] = '\0';

        // Add the value to the font substitute list
        da_append(&font_substitute_list, val);
    }

    // Open the font substitutes registry path
    code = RegOpenKeyExA(HKEY_LOCAL_MACHINE, FONT_SUBSTITUTES_REGISTRY_PATH, 0, KEY_READ, &font_substitutes_key);
    if (code != ERROR_SUCCESS) {
        nob_log(NOB_ERROR, "Failed to open key %s: %ld", FONT_SUBSTITUTES_REGISTRY_PATH, code);
        return_defer(40);
    }
    // Read the font substitutes registry path
    if (!reg_key_list_values(font_substitutes_key, &font_substitute_list)) return_defer(1);
    nob_log(NOB_INFO, "Amount of font substitutes: %zu", font_substitute_list.count);

    String_Builder font_reg = {0};
    // If a backup already exists, don't overwrite it
    if (file_exists(temp_sprintf("%s/%s", exe_dir, BACKUP_FONTS_REG_FILENAME))
    ) {
        nob_log(NOB_WARNING, "A backup already exists! Not overwriting the file.");
    } else {
        // Write the pre-modified registry values to a string builder
        if (!reg_key_get_file(FONTS_REGISTRY_PATH, font_list, &font_reg)) return 1;
        if (!reg_key_add_to_file(FONT_SUBSTITUTES_REGISTRY_PATH, font_substitute_list, &font_reg)) return 1;
        if (!reg_key_add_to_file(FONT_LINK_REGISTRY_PATH, font_link_list, &font_reg)) return 1;
        // Construct the backup file path
        char* fonts_backup_file_path = temp_sprintf("%s/%s", exe_dir, BACKUP_FONTS_REG_FILENAME);
        // Write the string builder to the backup file
        if (!write_entire_file(fonts_backup_file_path, font_reg.items, font_reg.count)) return false;
        nob_log(NOB_INFO, "Wrote fonts backup file to %s", fonts_backup_file_path);
        // Reset the temporary buffer, because it is used a lot in the reg_key_*_file functions
        temp_reset();
    }

    // Remove the font paths (except for the chosen font)
    for (size_t i = 0; i < font_list.count; ++i) {
        if (i == (size_t) font_index) continue;
        font_list.items[i].data = "";
        font_list.items[i].data_len = 0;
    }
    // Set the font substitute to the chosen font
    for (size_t i = 0; i < font_substitute_list.count; ++i) {
        if (i == (size_t) font_index) continue;
        font_substitute_list.items[i].data = font_substitute_list.items[font_index].name;
        font_substitute_list.items[i].data_len = font_substitute_list.items[font_index].name_len;
        font_substitute_list.items[i].type = REG_TYPE_STRING;
    }
    // Delete the font links
    for (size_t i = 0; i < font_link_list.count; ++i) {
        font_link_list.items[i].type = REG_TYPE_DELETE;
    }

    // Write the modified registry values to a string builder
    if (!reg_key_get_file(FONTS_REGISTRY_PATH, font_list, &font_reg)) return 1;
    if (!reg_key_add_to_file(FONT_SUBSTITUTES_REGISTRY_PATH, font_substitute_list, &font_reg)) return 1;
    if (!reg_key_add_to_file(FONT_LINK_REGISTRY_PATH, font_link_list, &font_reg)) return 1;
    // Construct the font-changing .reg file path
    char* fonts_backup_file_path = temp_sprintf("%s/fonts_%s.reg", exe_dir, font_list.items[font_index].name);
    // Write the string builder to said path
    if (!write_entire_file(fonts_backup_file_path, font_reg.items, font_reg.count)) return false;
    nob_log(NOB_INFO, "Wrote fonts registry file to %s", fonts_backup_file_path);
    // Reset the temporary buffer, because it is used a lot in the reg_key_*_file functions
    temp_reset();

    // Give some instructions on what to do in order to actually change the fonts
    printf("\n\n");
    printf("You can now import the generated fonts_%s.reg file.\n", font_list.items[font_index].name);
    printf("To restore things to normal, import the "BACKUP_FONTS_REG_FILENAME" file.\n");
    printf("Have fun!\n");
    printf("\n");

defer:
    // Cleanup
    if (fonts_key) RegCloseKey(fonts_key);
    if (font_substitutes_key) RegCloseKey(font_substitutes_key);
    if (font_link_key) RegCloseKey(font_link_key);
    return result;
}
