# wp_plugin_boilerplate
Описание: каркас корректного плагина WordPress
При создании плагина:
- Заголовок в главном файле: `<?php /** Plugin Name: ... Plugin URI: ... Description: ... Version: ... Author: ... */`.
- Проверяй окружение: `if ( ! defined( 'ABSPATH' ) ) exit;` в начале.
- Не используй префикс `wp_` в своих функциях/опциях (зарезервировано ядром).
- Регистрируй хуки на `init`/`plugins_loaded`; хуки активации — `register_activation_hook( __FILE__, 'my_activate' )`.
- Для опций используй `get_option()/update_option()`; для групп — `register_setting()`.
- Логируй через `error_log()` или WP-функцию отладки, не выводи в браузер.
