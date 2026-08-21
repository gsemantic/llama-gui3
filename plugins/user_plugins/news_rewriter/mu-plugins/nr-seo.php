<?php
/**
 * news_rewriter — минимальный SEO-рендерер (замена Yoast/RankMath).
 *
 * Читает postmeta nr_seo_* (заполняются плагином news_rewriter при публикации)
 * и выводит в <head>: <title>, meta description, OG/Twitter, canonical.
 *
 * Установка: скопировать этот файл в wp-content/mu-plugins/nr-seo.php
 * (создать папку mu-plugins, если её нет). Без настроек и UI.
 *
 * ВАЖНО: WordPress REST API (именно через него news_rewriter публикует посты)
 * принимает произвольные мета-поля ТОЛЬКО если они зарегистрированы с
 * show_in_rest => true. Без регистрации WP молча отбрасывает nr_seo_* и
 * get_post_meta вернёт пустоту. Поэтому ключи регистрируются ниже.
 */

foreach (array('nr_seo_title', 'nr_seo_description', 'nr_seo_keyword') as $key) {
    register_post_meta('post', $key, array(
        'type'         => 'string',
        'single'       => true,
        'show_in_rest' => true,
        'auth_callback' => function () {
            return current_user_can('edit_posts');
        },
    ));
}

add_filter('document_title_parts', function ($parts) {
    $t = get_post_meta(get_the_ID(), 'nr_seo_title', true);
    if (!empty($t)) {
        $parts['title'] = $t;
    }
    return $parts;
});

add_action('wp_head', function () {
    if (!is_singular()) {
        return;
    }
    $id = get_the_ID();
    if (!$id) {
        return;
    }

    $desc = get_post_meta($id, 'nr_seo_description', true);
    $kw   = get_post_meta($id, 'nr_seo_keyword', true);
    $url  = get_permalink($id);
    $img  = get_the_post_thumbnail_url($id, 'full');

    if (!empty($desc)) {
        echo '<meta name="description" content="' . esc_attr($desc) . '">' . "\n";
        echo '<meta property="og:description" content="' . esc_attr($desc) . '">' . "\n";
        echo '<meta name="twitter:description" content="' . esc_attr($desc) . '">' . "\n";
    }
    // og:title / twitter:title — из nr_seo_title, иначе из заголовка записи.
    $og_title = !empty($t) ? $t : get_the_title($id);
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
