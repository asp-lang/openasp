<?php

declare(strict_types=1);

require __DIR__ . '/bootstrap.php';

$database = blog_db();
$selected = trim((string)($_GET['tag'] ?? ''));
$content = '<h2>Tag cloud</h2><div>';
foreach (blog_tags($database) as $tag) {
    $size = 13 + min(16, (int)$tag['post_count']);
    $name = (string)$tag['name'];
    $content .= '<a style="font-size:' . $size . 'px;margin-right:14px" href="/tags.php?tag=' .
        rawurlencode($name) . '">' . blog_escape($name) . '</a>';
}
$content .= '</div>';

if ($selected !== '') {
    $statement = $database->prepare(
        "SELECT p.id, p.title, p.slug, p.summary, p.body, p.published_at, p.views,
                COALESCE(GROUP_CONCAT(all_tags.name ORDER BY all_tags.name SEPARATOR ','), '') AS tag_names
         FROM posts p
         JOIN post_tags selected_tags ON selected_tags.post_id = p.id
         JOIN tags selected_tag ON selected_tag.id = selected_tags.tag_id
         LEFT JOIN post_tags all_post_tags ON all_post_tags.post_id = p.id
         LEFT JOIN tags all_tags ON all_tags.id = all_post_tags.tag_id
         WHERE selected_tag.name = :tag
         GROUP BY p.id, p.title, p.slug, p.summary, p.body, p.published_at, p.views
         ORDER BY p.published_at DESC
         LIMIT 20"
    );
    $statement->execute(['tag' => $selected]);
    foreach ($statement as $post) {
        $content .= blog_render_post($post);
    }
}
blog_render_page('Tags', $content, $database);
