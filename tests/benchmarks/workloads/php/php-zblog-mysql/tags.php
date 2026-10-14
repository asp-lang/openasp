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
        'SELECT p.id, p.title, p.slug, p.summary, p.body, p.published_at, p.views
         FROM posts p
         JOIN post_tags pt ON pt.post_id = p.id
         JOIN tags t ON t.id = pt.tag_id
         WHERE t.name = :tag
         ORDER BY p.published_at DESC
         LIMIT 20'
    );
    $statement->execute(['tag' => $selected]);
    foreach ($statement as $post) {
        $content .= blog_render_post($database, $post);
    }
}
blog_render_page('Tags', $content, $database);

