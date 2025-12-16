#include "python_adapter_bridge.h"
#include "../utils/debug_utils.h"
#include <algorithm>
#include <filesystem>
#include <fstream>

#ifdef USE_PYTHON_ADAPTERS
// Include Python.h only when Python adapters are enabled
// Note: Python.h must be included before any standard headers on some platforms
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#endif

namespace ump {

PythonAdapterBridge& PythonAdapterBridge::Instance() {
    static PythonAdapterBridge instance;
    return instance;
}

PythonAdapterBridge::~PythonAdapterBridge() {
    Shutdown();
}

bool PythonAdapterBridge::Initialize(const std::string& python_home) {
#ifdef USE_PYTHON_ADAPTERS
    std::lock_guard<std::mutex> lock(mutex_);

    if (initialized_) {
        Debug::Log("PythonAdapterBridge: Already initialized");
        return true;
    }

    Debug::Log("PythonAdapterBridge: Initializing Python from: " + python_home);

    // Verify the Python home directory exists
    if (!std::filesystem::exists(python_home)) {
        last_error_ = "Python home directory not found: " + python_home;
        Debug::Log("PythonAdapterBridge: " + last_error_);
        return false;
    }

    python_home_ = python_home;

    // Use PyConfig for Python 3.8+ initialization (required for embedded Python)
    PyConfig config;
    PyConfig_InitIsolatedConfig(&config);

    // Convert paths to wide strings
    std::wstring wide_home(python_home.begin(), python_home.end());
    std::wstring program_name = wide_home + L"\\python.exe";
    std::wstring stdlib_zip = wide_home + L"\\python311.zip";

    // Set Python home and program name
    PyConfig_SetString(&config, &config.home, wide_home.c_str());
    PyConfig_SetString(&config, &config.program_name, program_name.c_str());

    // Disable site module initially (we'll set up paths manually)
    config.site_import = 0;

    // Initialize Python with config
    PyStatus status = Py_InitializeFromConfig(&config);
    PyConfig_Clear(&config);

    if (PyStatus_Exception(status)) {
        last_error_ = "Failed to initialize Python interpreter";
        if (status.err_msg) {
            last_error_ += std::string(": ") + status.err_msg;
        }
        Debug::Log("PythonAdapterBridge: " + last_error_);
        return false;
    }

    if (!Py_IsInitialized()) {
        last_error_ = "Failed to initialize Python interpreter";
        Debug::Log("PythonAdapterBridge: " + last_error_);
        return false;
    }

    Debug::Log("PythonAdapterBridge: Python interpreter initialized");

    // Set up sys.path to include stdlib zip and site-packages
    std::string setup_path =
        "import sys\n"
        "python_home = r'" + python_home + "'\n"
        "stdlib_zip = python_home + r'\\python311.zip'\n"
        "site_packages = python_home + r'\\Lib\\site-packages'\n"
        "if stdlib_zip not in sys.path:\n"
        "    sys.path.insert(0, stdlib_zip)\n"
        "if python_home not in sys.path:\n"
        "    sys.path.insert(0, python_home)\n"
        "if site_packages not in sys.path:\n"
        "    sys.path.append(site_packages)\n";

    PyRun_SimpleString(setup_path.c_str());

    // Import sys module for error handling
    sys_module_ = PyImport_ImportModule("sys");
    if (!sys_module_) {
        last_error_ = "Failed to import sys module";
        Debug::Log("PythonAdapterBridge: " + last_error_);
        if (PyErr_Occurred()) PyErr_Print();
        Py_Finalize();
        return false;
    }

    // Import OTIO adapters module
    adapters_module_ = PyImport_ImportModule("opentimelineio.adapters");
    if (!adapters_module_) {
        last_error_ = "Failed to import opentimelineio.adapters module. "
                      "Ensure opentimelineio is installed in the Python distribution.";
        Debug::Log("PythonAdapterBridge: " + last_error_);
        if (PyErr_Occurred()) PyErr_Print();
        Py_DECREF(static_cast<PyObject*>(sys_module_));
        sys_module_ = nullptr;
        Py_Finalize();
        return false;
    }

    Debug::Log("PythonAdapterBridge: OTIO adapters module loaded successfully");
    initialized_ = true;
    return true;

#else
    last_error_ = "Python adapters not enabled at compile time";
    Debug::Log("PythonAdapterBridge: " + last_error_);
    return false;
#endif
}

void PythonAdapterBridge::Shutdown() {
#ifdef USE_PYTHON_ADAPTERS
    std::lock_guard<std::mutex> lock(mutex_);

    if (!initialized_) return;

    Debug::Log("PythonAdapterBridge: Shutting down Python interpreter");

    if (adapters_module_) {
        Py_DECREF(static_cast<PyObject*>(adapters_module_));
        adapters_module_ = nullptr;
    }

    if (sys_module_) {
        Py_DECREF(static_cast<PyObject*>(sys_module_));
        sys_module_ = nullptr;
    }

    if (Py_IsInitialized()) {
        Py_Finalize();
    }

    initialized_ = false;
#endif
}

std::string PythonAdapterBridge::ImportTimeline(const std::string& file_path,
                                                 std::string& error_message) {
#ifdef USE_PYTHON_ADAPTERS
    std::lock_guard<std::mutex> lock(mutex_);

    if (!initialized_) {
        error_message = "Python interpreter not initialized";
        return "";
    }

    Debug::Log("PythonAdapterBridge: Importing timeline: " + file_path);

    // Verify file exists
    if (!std::filesystem::exists(file_path)) {
        error_message = "File not found: " + file_path;
        return "";
    }

    // Normalize path (forward slashes for Python)
    std::string normalized_path = file_path;
    std::replace(normalized_path.begin(), normalized_path.end(), '\\', '/');

    // Determine adapter name for XML files (OTIO doesn't auto-detect .xml)
    std::string adapter_name;
    std::string ext = std::filesystem::path(file_path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    if (ext == ".xml") {
        // Try to detect FCP7 vs FCPX by peeking at file content
        // FCP7 XML starts with <?xml...><xmeml...>
        // FCPX XML starts with <?xml...><!DOCTYPE fcpxml...> or <fcpxml...>
        std::ifstream file(file_path);
        if (file.is_open()) {
            std::string first_lines;
            std::string line;
            int lines_read = 0;
            while (std::getline(file, line) && lines_read < 10) {
                first_lines += line;
                lines_read++;
            }
            file.close();

            // Convert to lowercase for comparison
            std::string lower_content = first_lines;
            std::transform(lower_content.begin(), lower_content.end(), lower_content.begin(), ::tolower);

            if (lower_content.find("<fcpxml") != std::string::npos ||
                lower_content.find("<!doctype fcpxml") != std::string::npos) {
                adapter_name = "fcpx_xml";
                Debug::Log("PythonAdapterBridge: Detected FCPX XML format");
            } else if (lower_content.find("<xmeml") != std::string::npos) {
                adapter_name = "fcp_xml";
                Debug::Log("PythonAdapterBridge: Detected FCP7 XML format");
            } else {
                // Default to FCP7 XML (more common for Premiere exports)
                adapter_name = "fcp_xml";
                Debug::Log("PythonAdapterBridge: Defaulting to FCP7 XML format");
            }
        }
    }

    // Get the read_from_file function
    PyObject* read_func = PyObject_GetAttrString(
        static_cast<PyObject*>(adapters_module_), "read_from_file");

    if (!read_func) {
        error_message = "Failed to get read_from_file function";
        if (PyErr_Occurred()) PyErr_Print();
        return "";
    }

    // Create Python arguments
    PyObject* py_path = PyUnicode_FromString(normalized_path.c_str());
    if (!py_path) {
        error_message = "Failed to create Python string for path";
        Py_DECREF(read_func);
        return "";
    }

    PyObject* timeline = nullptr;

    if (!adapter_name.empty()) {
        // Call read_from_file(path, adapter_name=adapter_name)
        PyObject* kwargs = PyDict_New();
        PyObject* py_adapter = PyUnicode_FromString(adapter_name.c_str());
        PyDict_SetItemString(kwargs, "adapter_name", py_adapter);
        Py_DECREF(py_adapter);

        PyObject* args = PyTuple_Pack(1, py_path);
        timeline = PyObject_Call(read_func, args, kwargs);
        Py_DECREF(args);
        Py_DECREF(kwargs);
    } else {
        // Call read_from_file(path) - let OTIO auto-detect
        timeline = PyObject_CallFunctionObjArgs(read_func, py_path, NULL);
    }

    Py_DECREF(py_path);
    Py_DECREF(read_func);

    if (!timeline) {
        // Extract Python exception message
        if (PyErr_Occurred()) {
            PyObject *ptype, *pvalue, *ptraceback;
            PyErr_Fetch(&ptype, &pvalue, &ptraceback);
            PyErr_NormalizeException(&ptype, &pvalue, &ptraceback);

            if (pvalue) {
                PyObject* str = PyObject_Str(pvalue);
                if (str) {
                    const char* err_str = PyUnicode_AsUTF8(str);
                    if (err_str) {
                        error_message = err_str;
                    }
                    Py_DECREF(str);
                }
            }

            Py_XDECREF(ptype);
            Py_XDECREF(pvalue);
            Py_XDECREF(ptraceback);
        }

        if (error_message.empty()) {
            error_message = "Failed to read timeline file";
        }
        return "";
    }

    // Check if result is a Timeline object
    PyObject* timeline_type = PyObject_Type(timeline);
    PyObject* type_name = PyObject_GetAttrString(timeline_type, "__name__");
    if (type_name) {
        const char* name = PyUnicode_AsUTF8(type_name);
        Debug::Log("PythonAdapterBridge: Read object type: " + std::string(name ? name : "unknown"));
        Py_DECREF(type_name);
    }
    Py_DECREF(timeline_type);

    // Get to_json_string method
    PyObject* to_json_func = PyObject_GetAttrString(timeline, "to_json_string");
    if (!to_json_func) {
        error_message = "Timeline object does not have to_json_string method";
        Py_DECREF(timeline);
        if (PyErr_Occurred()) PyErr_Print();
        return "";
    }

    // Call to_json_string()
    PyObject* json_result = PyObject_CallObject(to_json_func, NULL);
    Py_DECREF(to_json_func);

    if (!json_result) {
        error_message = "Failed to serialize timeline to JSON";
        Py_DECREF(timeline);
        if (PyErr_Occurred()) PyErr_Print();
        return "";
    }

    // Extract JSON string
    const char* json_str = PyUnicode_AsUTF8(json_result);
    std::string result;
    if (json_str) {
        result = json_str;
        Debug::Log("PythonAdapterBridge: Successfully imported timeline (" +
                   std::to_string(result.length()) + " bytes JSON)");
    } else {
        error_message = "Failed to extract JSON string";
    }

    Py_DECREF(json_result);
    Py_DECREF(timeline);

    return result;

#else
    error_message = "Python adapters not enabled at compile time";
    return "";
#endif
}

bool PythonAdapterBridge::IsSupported(const std::string& file_path) {
    // Get file extension (lowercase)
    std::string ext;
    size_t dot_pos = file_path.rfind('.');
    if (dot_pos != std::string::npos) {
        ext = file_path.substr(dot_pos);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    }

    // Python adapters support these formats
    return ext == ".aaf" || ext == ".xml";
}

std::map<std::string, std::string> PythonAdapterBridge::ResolveMobPaths(
    const std::string& aaf_path,
    const std::vector<std::string>& mob_ids,
    const std::string& search_directory,
    std::string& error_message)
{
    std::map<std::string, std::string> result;

#ifdef USE_PYTHON_ADAPTERS
    std::lock_guard<std::mutex> lock(mutex_);

    if (!initialized_) {
        error_message = "Python interpreter not initialized";
        return result;
    }

    if (mob_ids.empty()) {
        Debug::Log("PythonAdapterBridge::ResolveMobPaths: No MobIDs to resolve");
        return result;  // Nothing to resolve
    }

    Debug::Log("PythonAdapterBridge::ResolveMobPaths: Resolving " +
               std::to_string(mob_ids.size()) + " MobIDs from " + aaf_path);
    Debug::Log("PythonAdapterBridge::ResolveMobPaths: Search directory: " + search_directory);

    // Normalize paths for Python (forward slashes)
    std::string normalized_aaf_path = aaf_path;
    std::replace(normalized_aaf_path.begin(), normalized_aaf_path.end(), '\\', '/');

    std::string normalized_search_dir = search_directory;
    std::replace(normalized_search_dir.begin(), normalized_search_dir.end(), '\\', '/');

    // Build JSON array of mob_ids with proper escaping
    std::string mob_ids_json = "[";
    for (size_t i = 0; i < mob_ids.size(); i++) {
        if (i > 0) mob_ids_json += ",";
        // Escape any quotes in the mob_id (shouldn't be any, but be safe)
        std::string escaped_id = mob_ids[i];
        size_t pos = 0;
        while ((pos = escaped_id.find('"', pos)) != std::string::npos) {
            escaped_id.replace(pos, 1, "\\\"");
            pos += 2;
        }
        mob_ids_json += "\"" + escaped_id + "\"";
    }
    mob_ids_json += "]";

    Debug::Log("PythonAdapterBridge::ResolveMobPaths: mob_ids_json = " + mob_ids_json);

    // Import our resolver module
    PyObject* resolver_module = PyImport_ImportModule("resolve_aaf_mobs");
    if (!resolver_module) {
        error_message = "Failed to import resolve_aaf_mobs module";
        Debug::Log("PythonAdapterBridge::ResolveMobPaths: " + error_message);
        if (PyErr_Occurred()) PyErr_Print();
        return result;
    }

    // Get the resolve_mob_paths_json function
    PyObject* resolve_func = PyObject_GetAttrString(resolver_module, "resolve_mob_paths_json");
    if (!resolve_func) {
        error_message = "Failed to get resolve_mob_paths_json function";
        Debug::Log("PythonAdapterBridge::ResolveMobPaths: " + error_message);
        Py_DECREF(resolver_module);
        if (PyErr_Occurred()) PyErr_Print();
        return result;
    }

    // Create Python arguments
    PyObject* py_aaf_path = PyUnicode_FromString(normalized_aaf_path.c_str());
    PyObject* py_mob_ids = PyUnicode_FromString(mob_ids_json.c_str());
    PyObject* py_search_dir = PyUnicode_FromString(normalized_search_dir.c_str());

    if (!py_aaf_path || !py_mob_ids || !py_search_dir) {
        error_message = "Failed to create Python strings";
        Debug::Log("PythonAdapterBridge::ResolveMobPaths: " + error_message);
        Py_XDECREF(py_aaf_path);
        Py_XDECREF(py_mob_ids);
        Py_XDECREF(py_search_dir);
        Py_DECREF(resolve_func);
        Py_DECREF(resolver_module);
        return result;
    }

    // Call resolve_mob_paths_json(aaf_path, mob_ids_json, search_directory)
    PyObject* json_result = PyObject_CallFunctionObjArgs(
        resolve_func, py_aaf_path, py_mob_ids, py_search_dir, NULL);

    Py_DECREF(py_aaf_path);
    Py_DECREF(py_mob_ids);
    Py_DECREF(py_search_dir);
    Py_DECREF(resolve_func);
    Py_DECREF(resolver_module);

    if (!json_result) {
        error_message = "Mob resolution call failed";
        Debug::Log("PythonAdapterBridge::ResolveMobPaths: " + error_message);
        if (PyErr_Occurred()) PyErr_Print();
        return result;
    }

    // Extract JSON string result
    const char* json_str = PyUnicode_AsUTF8(json_result);
    if (!json_str) {
        error_message = "Failed to extract JSON result string";
        Debug::Log("PythonAdapterBridge::ResolveMobPaths: " + error_message);
        Py_DECREF(json_result);
        return result;
    }

    std::string json_response = json_str;
    Py_DECREF(json_result);

    Debug::Log("PythonAdapterBridge::ResolveMobPaths: Response: " + json_response);

    // Parse JSON response into result map
    // Format: {"mob_id1": "path1", "mob_id2": "path2", ...}
    // Simple parsing without external JSON library
    if (json_response.size() >= 2 && json_response[0] == '{') {
        // Skip opening brace
        size_t pos = 1;
        while (pos < json_response.size()) {
            // Skip whitespace
            while (pos < json_response.size() && std::isspace(json_response[pos])) pos++;

            // End of object?
            if (pos >= json_response.size() || json_response[pos] == '}') break;

            // Expect opening quote for key
            if (json_response[pos] != '"') {
                pos++;
                continue;
            }
            pos++; // Skip opening quote

            // Find key end
            size_t key_start = pos;
            while (pos < json_response.size() && json_response[pos] != '"') {
                if (json_response[pos] == '\\') pos++; // Skip escaped char
                pos++;
            }
            std::string key = json_response.substr(key_start, pos - key_start);
            pos++; // Skip closing quote

            // Skip colon and whitespace
            while (pos < json_response.size() && (json_response[pos] == ':' || std::isspace(json_response[pos]))) pos++;

            // Expect opening quote for value
            if (pos >= json_response.size() || json_response[pos] != '"') {
                // Skip to next comma or end
                while (pos < json_response.size() && json_response[pos] != ',' && json_response[pos] != '}') pos++;
                if (json_response[pos] == ',') pos++;
                continue;
            }
            pos++; // Skip opening quote

            // Find value end
            size_t value_start = pos;
            while (pos < json_response.size() && json_response[pos] != '"') {
                if (json_response[pos] == '\\') pos++; // Skip escaped char
                pos++;
            }
            std::string value = json_response.substr(value_start, pos - value_start);
            pos++; // Skip closing quote

            // Unescape backslashes in the path
            std::string unescaped_value;
            for (size_t i = 0; i < value.size(); i++) {
                if (value[i] == '\\' && i + 1 < value.size()) {
                    char next = value[i + 1];
                    if (next == '\\' || next == '"' || next == '/') {
                        unescaped_value += next;
                        i++;
                        continue;
                    }
                }
                unescaped_value += value[i];
            }

            // Store in result map (only non-empty paths)
            if (!unescaped_value.empty()) {
                result[key] = unescaped_value;
                Debug::Log("PythonAdapterBridge::ResolveMobPaths: Resolved '" +
                           key.substr(0, 40) + "...' -> " + unescaped_value);
            }

            // Skip comma
            while (pos < json_response.size() && (json_response[pos] == ',' || std::isspace(json_response[pos]))) pos++;
        }
    }

    Debug::Log("PythonAdapterBridge::ResolveMobPaths: Resolved " +
               std::to_string(result.size()) + "/" + std::to_string(mob_ids.size()) + " paths");

#else
    error_message = "Python adapters not enabled at compile time";
#endif

    return result;
}

PythonAdapterBridge::AllMobsResult PythonAdapterBridge::ResolveAllMobs(
    const std::string& aaf_path,
    const std::string& search_directory,
    std::string& error_message)
{
    AllMobsResult result;

#ifdef USE_PYTHON_ADAPTERS
    std::lock_guard<std::mutex> lock(mutex_);

    if (!initialized_) {
        error_message = "Python interpreter not initialized";
        return result;
    }

    Debug::Log("PythonAdapterBridge::ResolveAllMobs: Resolving all MasterMobs from " + aaf_path);
    Debug::Log("PythonAdapterBridge::ResolveAllMobs: Search directory: " + search_directory);

    // Normalize paths for Python (forward slashes)
    std::string normalized_aaf_path = aaf_path;
    std::replace(normalized_aaf_path.begin(), normalized_aaf_path.end(), '\\', '/');

    std::string normalized_search_dir = search_directory;
    std::replace(normalized_search_dir.begin(), normalized_search_dir.end(), '\\', '/');

    // Import our resolver module
    PyObject* resolver_module = PyImport_ImportModule("resolve_aaf_mobs");
    if (!resolver_module) {
        error_message = "Failed to import resolve_aaf_mobs module";
        Debug::Log("PythonAdapterBridge::ResolveAllMobs: " + error_message);
        if (PyErr_Occurred()) PyErr_Print();
        return result;
    }

    // Get the resolve_all_mobs_json function
    PyObject* resolve_func = PyObject_GetAttrString(resolver_module, "resolve_all_mobs_json");
    if (!resolve_func) {
        error_message = "Failed to get resolve_all_mobs_json function";
        Debug::Log("PythonAdapterBridge::ResolveAllMobs: " + error_message);
        Py_DECREF(resolver_module);
        if (PyErr_Occurred()) PyErr_Print();
        return result;
    }

    // Create Python arguments
    PyObject* py_aaf_path = PyUnicode_FromString(normalized_aaf_path.c_str());
    PyObject* py_search_dir = PyUnicode_FromString(normalized_search_dir.c_str());

    if (!py_aaf_path || !py_search_dir) {
        error_message = "Failed to create Python strings";
        Debug::Log("PythonAdapterBridge::ResolveAllMobs: " + error_message);
        Py_XDECREF(py_aaf_path);
        Py_XDECREF(py_search_dir);
        Py_DECREF(resolve_func);
        Py_DECREF(resolver_module);
        return result;
    }

    // Call resolve_all_mobs_json(aaf_path, search_directory)
    PyObject* json_result = PyObject_CallFunctionObjArgs(
        resolve_func, py_aaf_path, py_search_dir, NULL);

    Py_DECREF(py_aaf_path);
    Py_DECREF(py_search_dir);
    Py_DECREF(resolve_func);
    Py_DECREF(resolver_module);

    if (!json_result) {
        error_message = "ResolveAllMobs call failed";
        Debug::Log("PythonAdapterBridge::ResolveAllMobs: " + error_message);
        if (PyErr_Occurred()) PyErr_Print();
        return result;
    }

    // Extract JSON string result
    const char* json_str = PyUnicode_AsUTF8(json_result);
    if (!json_str) {
        error_message = "Failed to extract JSON result string";
        Debug::Log("PythonAdapterBridge::ResolveAllMobs: " + error_message);
        Py_DECREF(json_result);
        return result;
    }

    std::string json_response = json_str;
    Py_DECREF(json_result);

    Debug::Log("PythonAdapterBridge::ResolveAllMobs: Response length: " +
               std::to_string(json_response.length()) + " bytes");

    // Parse JSON response: {"by_mob_id": {...}, "by_name": {...}}
    // Simple parsing - look for the sub-objects
    auto parseSubObject = [](const std::string& json, const std::string& key) -> std::map<std::string, std::string> {
        std::map<std::string, std::string> map;

        // Find "key": {
        std::string search_key = "\"" + key + "\":";
        size_t start = json.find(search_key);
        if (start == std::string::npos) return map;

        start += search_key.length();
        // Skip whitespace
        while (start < json.size() && std::isspace(json[start])) start++;

        // Must be {
        if (start >= json.size() || json[start] != '{') return map;
        start++; // Skip {

        // Parse key-value pairs until }
        while (start < json.size()) {
            // Skip whitespace
            while (start < json.size() && std::isspace(json[start])) start++;

            // End of object?
            if (start >= json.size() || json[start] == '}') break;

            // Expect "
            if (json[start] != '"') {
                start++;
                continue;
            }
            start++; // Skip opening quote

            // Find key end
            size_t key_start = start;
            while (start < json.size() && json[start] != '"') {
                if (json[start] == '\\') start++; // Skip escaped char
                start++;
            }
            std::string k = json.substr(key_start, start - key_start);
            start++; // Skip closing quote

            // Skip : and whitespace
            while (start < json.size() && (json[start] == ':' || std::isspace(json[start]))) start++;

            // Expect "
            if (start >= json.size() || json[start] != '"') {
                // Skip to next comma or }
                while (start < json.size() && json[start] != ',' && json[start] != '}') start++;
                if (json[start] == ',') start++;
                continue;
            }
            start++; // Skip opening quote

            // Find value end
            size_t value_start = start;
            while (start < json.size() && json[start] != '"') {
                if (json[start] == '\\') start++; // Skip escaped char
                start++;
            }
            std::string v = json.substr(value_start, start - value_start);
            start++; // Skip closing quote

            // Unescape backslashes
            std::string unescaped;
            for (size_t i = 0; i < v.size(); i++) {
                if (v[i] == '\\' && i + 1 < v.size()) {
                    char next = v[i + 1];
                    if (next == '\\' || next == '"' || next == '/') {
                        unescaped += next;
                        i++;
                        continue;
                    }
                }
                unescaped += v[i];
            }

            if (!unescaped.empty()) {
                map[k] = unescaped;
            }

            // Skip comma
            while (start < json.size() && (json[start] == ',' || std::isspace(json[start]))) start++;
        }

        return map;
    };

    result.by_mob_id = parseSubObject(json_response, "by_mob_id");
    result.by_name = parseSubObject(json_response, "by_name");

    Debug::Log("PythonAdapterBridge::ResolveAllMobs: Resolved " +
               std::to_string(result.by_mob_id.size()) + " by MobID, " +
               std::to_string(result.by_name.size()) + " by name");

#else
    error_message = "Python adapters not enabled at compile time";
#endif

    return result;
}

} // namespace ump
