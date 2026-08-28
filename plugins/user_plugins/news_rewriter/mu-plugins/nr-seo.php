<?php
/**
 * news_rewriter — мост SEO-разметки в WordPress.
 *
 * Читает postmeta nr_seo_* (заполняются плагином news_rewriter при публикации)
 * и:
 *   1) при сохранении/обновлении мета копирует их в мета плагина Yoast SEO
 *      (через WPSEO_Meta::set_value), чтобы Yoast «увидел» разметку: его
 *      метабокс/анализ и вывод <meta name="description">, title, OG/Twitter;
 *   2) преобразует «сырой» HTML контента (который шлёт рерайтер) в блоки
 *      Gutenberg (wp:paragraph/wp:heading/...), чтобы в редакторе не приходилось
 *      вручную делать «Преобразовать в блоки»;
 *   3) если Yoast SEO НЕ установлен ИЛИ у поста ещё нет мета Yoast — сам
 *      выводит минимальный набор тегов (description, OG/Twitter, canonical)
 *      в <head> как запасной вариант.
 *
 * Установка: скопировать этот файл в wp-content/mu-plugins/nr-seo.php
 * (создать папку mu-plugins, если её нет). Без настроек и UI.
 *
 * ВАЖНО: WordPress REST API (именно через него news_rewriter публикует посты)
 * принимает произвольные мета-поля ТОЛЬКО если они зарегистрированы с
 * show_in_rest => true. Без регистрации WP молча отбрасывает nr_seo_* и
 * get_post_meta вернёт пустоту. Поэтому ключи регистрируются ниже.
 *
 * ВЕРСИЯ: 4 (маркер <!-- nr_seo_bridge v4 --> в <head> — для проверки, что
 * именно эта версия исполняется; если маркера нет — OPcache отдаёт старый код).
 */

foreach (array('post', 'page') as $pt) {
    foreach (array('nr_seo_title', 'nr_seo_description', 'nr_seo_keyword') as $key) {
        register_post_meta($pt, $key, array(
            'type'         => 'string',
            'single'       => true,
            'show_in_rest' => true,
            'auth_callback' => function () {
                return current_user_can('edit_posts');
            },
        ));
    }
}

// <title> страницы — из nr_seo_title (независимо от Yoast, значения совпадают).
add_filter('document_title_parts', function ($parts) {
    $t = get_post_meta(get_the_ID(), 'nr_seo_title', true);
    if (!empty($t)) {
        $parts['title'] = $t;
    }
    return $parts;
});

/**
 * Прокидывает nr_seo_* в мета Yoast SEO.
 * Используется API Yoast (WPSEO_Meta::set_value), чтобы не зависеть от формата
 * хранения конкретной версии плагина. Если Yoast не активен — тихо выходим.
 */
function nr_seo_log($msg) {
    nr_seo_rec($msg);
}
function nr_seo_rec($msg) {
    $cur = get_option('nr_seo_diag', '');
    update_option('nr_seo_diag', substr($cur . date('c') . ' ' . $msg . "\n", -4000));
}
add_action('admin_notices', function () {
    if (!current_user_can('manage_options')) return;
    $d = get_option('nr_seo_diag', '');
    if ($d === '') return;
    echo '<div class="notice notice-info is-dismissible"><p><strong>nr-seo diag:</strong></p><pre style="white-space:pre-wrap">'
         . esc_html($d) . '</pre></div>';
});
nr_seo_rec("LOADED v4d (wp=" . (defined('ABSPATH') ? 'yes' : 'no') . ")");

function nr_seo_sync_to_yoast($post_id) {
    nr_seo_log("sync pid=$post_id");
    if (defined('DOING_AUTOSAVE') && DOING_AUTOSAVE) { nr_seo_log("autosave skip"); return; }

    // Не затираем ручную разметку Yoast, если news_rewriter ничего не прислал.
    $title = get_post_meta($post_id, 'nr_seo_title', true);
    $desc  = get_post_meta($post_id, 'nr_seo_description', true);
    $kw    = get_post_meta($post_id, 'nr_seo_keyword', true);
    nr_seo_log("meta title=" . ($title !== '' ? 'Y' : 'N') .
               " desc=" . ($desc !== '' ? 'Y' : 'N') .
               " kw=" . ($kw !== '' ? 'Y' : 'N'));
    if ($title === '' && $desc === '' && $kw === '') { nr_seo_log("no seo meta, skip"); return; }

    // 1) Пишем в postmeta (legacy-источник, читается сборщиком Indexable).
    if (class_exists('WPSEO_Meta')) {
        if ($title !== '' && $title !== false) WPSEO_Meta::set_value('title', $title, $post_id);
        if ($desc  !== '' && $desc  !== false) WPSEO_Meta::set_value('metadesc', $desc, $post_id);
        if ($kw    !== '' && $kw    !== false) WPSEO_Meta::set_value('focuskw', $kw, $post_id);
        nr_seo_log("postmeta written; re-read metadesc=" . get_post_meta($post_id, '_yoast_wpseo_metadesc', true) .
                   " title=" . get_post_meta($post_id, '_yoast_wpseo_title', true) .
                   " focuskw=" . get_post_meta($post_id, '_yoast_wpseo_focuskw', true));
    } else { nr_seo_log("WPSEO_Meta missing!"); }

    // 2) Прямая запись в таблицу Indexable (именно её читает метабокс Yoast).
    global $wpdb;
    if (isset($wpdb) && method_exists($wpdb, 'get_var')) {
        $table = $wpdb->prefix . 'yoast_indexable';
        $cols = $wpdb->get_col("SHOW COLUMNS FROM $table");
        nr_seo_log("idx table=$table cols=" . implode(',', $cols));
        $idx_id = $wpdb->get_var(
            $wpdb->prepare("SELECT id FROM $table WHERE object_id=%d AND object_type='post' LIMIT 1", $post_id)
        );
        $set = array();
        if ($title !== '' && $title !== false) $set['title'] = $title;
        if ($desc  !== '' && $desc  !== false) $set['description'] = $desc;
        if ($kw    !== '' && $kw    !== false) $set['primary_focus_keyword'] = $kw;
        if ($idx_id) {
            $r = $wpdb->update($table, $set, array('id' => $idx_id));
            nr_seo_log("wpdb update idx_id=$idx_id res=" . var_export($r, true) . " err=" . $wpdb->last_error);
        } else {
            nr_seo_log("indexable row NOT found for pid=$post_id (builder will try to create)");
        }
    }

    // 3) Перестроить Indexable штатным сборщиком (создаёт строку, если нет, и
    //    обновляет связанные счётчики анализа).
    if (function_exists('YoastSEO')) {
        try {
            $builder = YoastSEO()->classes->get('Yoast\WP\SEO\Builders\Indexable_Builder');
            if (method_exists($builder, 'build_for_id_and_type')) {
                $builder->build_for_id_and_type($post_id, 'post');
                nr_seo_log("builder ok");
            }
        } catch (\Throwable $e) {
            nr_seo_log("builder exc: " . $e->getMessage());
        }
    } else { nr_seo_log("YoastSEO() missing!"); }
}

// Обычное сохранение (не через REST) — сработает, когда мета уже записана.
add_action('save_post', function ($post_id, $post = null) {
    nr_seo_sync_to_yoast($post_id);
}, 20, 2);

// REST-публикация news_rewriter: мета nr_seo_* пишется ПОСЛЕ срабатывания
// save_post, поэтому ловим именно момент записи/обновления самих nr_seo_*-ключей.
// Эти хуки гарантированно выполняются после того, как значение сохранено.
foreach (array('added_post_meta', 'updated_post_meta') as $meta_hook) {
    add_action($meta_hook, function ($meta_id, $object_id, $meta_key, $meta_value) {
        if (!in_array($meta_key, array('nr_seo_title', 'nr_seo_description', 'nr_seo_keyword'), true)) {
            return;
        }
        nr_seo_sync_to_yoast($object_id);
    }, 10, 4);
}

// REST-вставка: дополнительная страховка после сохранения всех мета (пост/страница).
foreach (array('post', 'page') as $pt) {
    add_action('rest_after_insert_' . $pt, function ($post, $request, $creating) {
        nr_seo_sync_to_yoast($post->ID);
    }, 10, 3);
}

/**
 * Преобразуем «сырой» HTML контента наших постов в блоки Gutenberg, чтобы в
 * редакторе не требовалось ручное «Преобразовать в блоки». Работаем только для
 * записей, помеченных nr_seo_* (т.е. опубликованных рерайтером), и только если
 * контент ещё не в формате блоков.
 */
$nr_seo_blockify = function ($post_id, $post = null) {
    if (defined('DOING_AUTOSAVE') && DOING_AUTOSAVE) return;
    if (wp_is_post_revision($post_id) || wp_is_post_autosave($post_id)) return;
    if (get_post_meta($post_id, 'nr_seo_description', true) === '') return; // не наш пост
    $content = get_post_field('post_content', $post_id);
    if ($content === '' || strpos($content, '<!-- wp:') !== false) return; // уже блоки
    if (!function_exists('parse_blocks') || !function_exists('serialize_blocks')) return;
    $new = serialize_blocks(parse_blocks($content));
    if ($new !== $content && $new !== '') {
        remove_action('save_post', $GLOBALS['nr_seo_blockify_ref'], 21);
        wp_update_post(array('ID' => $post_id, 'post_content' => $new));
        add_action('save_post', $GLOBALS['nr_seo_blockify_ref'], 21, 2);
    }
};
$GLOBALS['nr_seo_blockify_ref'] = $nr_seo_blockify;
add_action('save_post', $nr_seo_blockify, 21, 2);

add_action('wp_head', function () {
    // Маркер версии — подтверждает, что исполняется именно этот файл
    // (при включённом OPcache старый код не обновляется до сброса кеша).
    echo "<!-- nr_seo_bridge v4 -->\n";

    if (!is_singular()) {
        return;
    }
    $id = get_the_ID();
    if (!$id) {
        return;
    }

    // Если Yoast активен И у поста уже есть его мета-описание — Yoast сам выведет
    // description/title/OG/Twitter, дублировать не нужно.
    $yoast_has_desc = false;
    if (class_exists('WPSEO_Meta')) {
        $yd = WPSEO_Meta::get_value('metadesc', $id);
        $yoast_has_desc = !empty($yd);
    }
    if (class_exists('WPSEO_Frontend') && $yoast_has_desc) {
        return;
    }

    // Запасной рендер из nr_seo_* (Yoast не установлен либо мета ещё не
    // синхронизировалась — тогда хотя бы наши теги присутствуют на странице).
    $desc  = get_post_meta($id, 'nr_seo_description', true);
    $kw    = get_post_meta($id, 'nr_seo_keyword', true);
    $title = get_post_meta($id, 'nr_seo_title', true);
    $url   = get_permalink($id);
    $img   = get_the_post_thumbnail_url($id, 'full');

    if (!empty($desc)) {
        echo '<meta name="description" content="' . esc_attr($desc) . '">' . "\n";
        echo '<meta property="og:description" content="' . esc_attr($desc) . '">' . "\n";
        echo '<meta name="twitter:description" content="' . esc_attr($desc) . '">' . "\n";
    }
    $og_title = !empty($title) ? $title : get_the_title($id);
    if (!empty($og_title)) {
        echo '<meta property="og:title" content="' . esc_attr($og_title) . '">' . "\n";
        echo '<meta name="twitter:title" content="' . esc_attr($og_title) . '">' . "\n";
    }
    if (!empty($kw)) {
        echo '<meta name="keywords" content="' . esc_attr($kw) . '">' . "\n";
    }
    if (!empty($url)) {
        echo '<link rel="canonical" href="' . esc_url($url) . '">' . "\n";
        echo '<meta property="og:url" content="' . esc_url($url) . '">' . "\n";
    }
    echo '<meta property="og:type" content="article">' . "\n";
    if (!empty($img)) {
        echo '<meta property="og:image" content="' . esc_url($img) . '">' . "\n";
    }
    echo '<meta name="twitter:card" content="summary_large_image">' . "\n";
}, 1);
