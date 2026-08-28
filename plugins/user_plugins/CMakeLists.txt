# Пользовательские плагины — разрабатываются независимо от основного проекта.
# Корневой plugins/CMakeLists.txt подхватывает этот файл автоматически.
add_subdirectory(news_rewriter)
add_subdirectory(wp_coder)
