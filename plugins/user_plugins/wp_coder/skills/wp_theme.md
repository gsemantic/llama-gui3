# wp_theme
Описание: иерархия шаблонов и безопасная вёрстка темы
При правке темы WordPress:
- Точка входа — `style.css` (заголовок темы обязателен) и `index.php`.
- Подключай стили/скрипты только через `wp_enqueue_scripts` + `wp_enqueue_style/wp_enqueue_script`, НЕ через `<link>` в head.
- Используй цикл: `if ( have_posts() ) : while ( have_posts() ) : the_post(); ... endwhile; endif;`
- Экранируй вывод: `esc_html()`, `esc_attr()`, `esc_url()`; перевод — `__('...', 'textdomain')`.
- `get_template_part( 'content', 'page' )` для переиспользуемых блоков.
- Не правь `wp-includes`/`wp-admin` — только `wp-content/themes/<theme>` и `wp-content/plugins`.
