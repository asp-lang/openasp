<?php

declare(strict_types=1);

require __DIR__ . '/bootstrap.php';

$database = blog_db();
$postId = max(1, filter_input(INPUT_GET, 'id', FILTER_VALIDATE_INT) ?: 1);
$statement = $database->prepare(
    "SELECT p.id, p.title, p.slug, p.summary, p.body, p.published_at, p.views,
            COALESCE(GROUP_CONCAT(t.name ORDER BY t.name SEPARATOR ','), '') AS tag_names
     FROM posts p
     LEFT JOIN post_tags pt ON pt.post_id = p.id
     LEFT JOIN tags t ON t.id = pt.tag_id
     WHERE p.id = :id
     GROUP BY p.id, p.title, p.slug, p.summary, p.body, p.published_at, p.views"
);
$statement->execute(['id' => $postId]);
$post = $statement->fetch();
if ($post === false) {
    http_response_code(404);
    $content = '<h2>Article not found</h2>';
} else {
    $content = blog_render_post($post, true);
}
blog_render_page('Article', $content, $database);
