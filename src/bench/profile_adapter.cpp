#include "../../include/bench/profile_adapter.h"
#include "../../include/bench/bench_common.h"

#include <fstream>
#include <sstream>
#include <iostream>

namespace llama_gui {
namespace bench {

BenchTestParams ProfileAdapter::profileToBenchParams(const std::string& profile_path) {
    BenchTestParams params;
    
    // Загрузить JSON
    std::string content = BenchCommon::readFileToString(profile_path);
    if (content.empty()) {
        return params;
    }
    
    try {
        nlohmann::json json = nlohmann::json::parse(content);
        return profileToBenchParams(json);
    } catch (const std::exception&) {
        return params;
    }
}

BenchTestParams ProfileAdapter::profileToBenchParams(const nlohmann::json& profile_json) {
    BenchTestParams params;

    // Извлечь путь к модели
    params.model_path = extractModelPath(profile_json);
    params.model_name = extractModelName(params.model_path);

    std::cout << "[ProfileAdapter] Model path: " << params.model_path << std::endl;
    std::cout << "[ProfileAdapter] Model name: " << params.model_name << std::endl;

    // Извлечь параметры из batch секции
    if (profile_json.contains("batch")) {
        const auto& batch = profile_json["batch"];
        params.batch_size = batch.value("batch_size", 512);  // Более разумное значение по умолчанию
        params.threads = batch.value("threads", 2);
        params.ubatch_size = batch.value("ubatch_size", 256);  // Извлекаем ubatch_size
        params.n_depth.push_back(0);  // -d 0: тест без искусственного заполнения контекста

        std::cout << "[ProfileAdapter] batch_size=" << params.batch_size
                  << ", ubatch_size=" << params.ubatch_size
                  << ", threads=" << params.threads
                  << ", ctx_size=" << params.n_depth[0] << std::endl;
    } else {
        std::cout << "[ProfileAdapter] WARNING: 'batch' section not found in profile!" << std::endl;
    }

    // Извлечь параметры из chat секции
    if (profile_json.contains("chat")) {
        const auto& chat = profile_json["chat"];
        params.n_gpu_layers = chat.value("n_gpu_layers", 0);
        std::cout << "[ProfileAdapter] n_gpu_layers (chat)=" << params.n_gpu_layers << std::endl;
    }

    // Извлечь параметры из gpu секции
    if (profile_json.contains("gpu")) {
        const auto& gpu = profile_json["gpu"];
        int gpu_layers = gpu.value("n_gpu_layers", -1);
        if (gpu_layers >= 0) {
            params.n_gpu_layers = gpu_layers;
            std::cout << "[ProfileAdapter] n_gpu_layers (gpu)=" << params.n_gpu_layers << std::endl;
        }
    }

    // Извлечь параметры из cache секции
    if (profile_json.contains("cache")) {
        const auto& cache = profile_json["cache"];
        params.cache_type_k = cache.value("cache_type_k", "f16");  // f16 по умолчанию для совместимости
        params.cache_type_v = cache.value("cache_type_v", "f16");  // f16 по умолчанию для совместимости
        std::cout << "[ProfileAdapter] cache_type_k=" << params.cache_type_k
                  << ", cache_type_v=" << params.cache_type_v << std::endl;
    } else {
        std::cout << "[ProfileAdapter] WARNING: 'cache' section not found in profile!" << std::endl;
    }

    // Извлечь параметры из model_loading секции
    if (profile_json.contains("model_loading")) {
        const auto& model_loading = profile_json["model_loading"];
        std::string path = model_loading.value("model_path", "");
        if (!path.empty() && params.model_path.empty()) {
            params.model_path = path;
            params.model_name = extractModelName(path);
            std::cout << "[ProfileAdapter] Model path from model_loading: " << params.model_path << std::endl;
        }
    }

    // Извлечь параметры из output секции
    if (profile_json.contains("output")) {
        const auto& output = profile_json["output"];
        params.n_gen = output.value("n_predict", 512);  // Более разумное значение
        std::cout << "[ProfileAdapter] n_gen (n_predict)=" << params.n_gen << std::endl;
    }

    // Извлечь параметры из sampling секции
    if (profile_json.contains("sampling")) {
        const auto& sampling = profile_json["sampling"];
        params.repetitions = 3;  // Фиксированное значение для бенчмарка
    }

    // Проверка валидности
    if (params.model_path.empty()) {
        std::cerr << "[ProfileAdapter] ERROR: Model path is empty!" << std::endl;
    }

    return params;
}

std::string ProfileAdapter::extractModelPath(const nlohmann::json& profile_json) {
    // After resolveDuplicates, model_loading.model_path is synced with custom.model_path
    // Check model_loading first (authoritative), then custom as fallback
    if (profile_json.contains("model_loading")) {
        std::string path = profile_json["model_loading"].value("model_path", "");
        if (!path.empty()) {
            return path;
        }
    }
    if (profile_json.contains("custom")) {
        return profile_json["custom"].value("model_path", "");
    }
    return "";
}

std::string ProfileAdapter::extractModelName(const std::string& model_path) {
    if (model_path.empty()) {
        return "";
    }
    
    // Извлечь имя файла из пути
    size_t pos = model_path.find_last_of("/\\");
    std::string filename = (pos != std::string::npos) ? 
        model_path.substr(pos + 1) : model_path;
    
    // Удалить расширение
    size_t ext_pos = filename.find_last_of('.');
    if (ext_pos != std::string::npos) {
        filename = filename.substr(0, ext_pos);
    }
    
    return filename;
}

int ProfileAdapter::extractThreads(const nlohmann::json& json) {
    // After resolveDuplicates, batch.threads is authoritative
    if (json.contains("batch")) {
        return json["batch"].value("threads", 2);
    }
    return 2;
}

int ProfileAdapter::extractBatchSize(const nlohmann::json& json) {
    if (json.contains("batch")) {
        return json["batch"].value("batch_size", 2048);
    }
    return 2048;
}

int ProfileAdapter::extractGpuLayers(const nlohmann::json& json) {
    // After resolveDuplicates, gpu.n_gpu_layers is authoritative
    if (json.contains("gpu")) {
        return json["gpu"].value("n_gpu_layers", 0);
    }
    return 0;
}

int ProfileAdapter::extractContextSize(const nlohmann::json& json) {
    // After resolveDuplicates, chat.n_ctx is authoritative
    if (json.contains("chat")) {
        return json["chat"].value("n_ctx", 4096);
    }
    if (json.contains("batch")) {
        return json["batch"].value("ctx_size", 4096);
    }
    return 4096;
}

std::vector<std::string> ProfileAdapter::getAvailableProfiles(const std::string& profiles_dir) {
    std::vector<std::string> profiles;
    
    auto files = BenchCommon::listFilesWithExtension(profiles_dir, ".json");
    for (const auto& file : files) {
        std::string name = BenchCommon::extractFileNameWithoutExt(file);
        profiles.push_back(name);
    }
    
    return profiles;
}

nlohmann::json ProfileAdapter::loadProfile(const std::string& profile_name,
                                          const std::string& profiles_dir) {
    std::string path = profiles_dir + "/" + profile_name + ".json";
    
    std::string content = BenchCommon::readFileToString(path);
    if (content.empty()) {
        return nlohmann::json::object();
    }
    
    try {
        return nlohmann::json::parse(content);
    } catch (const std::exception&) {
        return nlohmann::json::object();
    }
}

bool ProfileAdapter::validateProfileForBenchmark(const nlohmann::json& profile_json,
                                                std::string& error_message) {
    std::string model_path = extractModelPath(profile_json);
    
    if (model_path.empty()) {
        error_message = "Model path not found in profile";
        return false;
    }
    
    if (!BenchCommon::fileExists(model_path)) {
        error_message = "Model file not found: " + model_path;
        return false;
    }
    
    return true;
}

} // namespace bench
} // namespace llama_gui
