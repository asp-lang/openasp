<?php

declare(strict_types=1);

require __DIR__ . '/bootstrap.php';

$database = blog_db();
$postId = max(1, filter_input(INPUT_GET, 'id', FILTER_VALIDATE_INT) ?: 1);
$statement = $database->prepare(
    'SELECT id, title, slug, summary, body, published_at, views
     FROM posts
     WHERE id = :id'
);
$statement->execute(['id' => $postId]);
$post = $statement->fetch();
if ($post === false) {
    http_response_code(404);
    $content = '<h2>Article not found</h2>';
} else {
    $content = blog_render_post($database, $post, true);
}
blog_render_page('Article', $content, $database);
