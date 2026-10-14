<?php

declare(strict_types=1);

require __DIR__ . '/bootstrap.php';

$database = blog_db();
$page = max(1, filter_input(INPUT_GET, 'page', FILTER_VALIDATE_INT) ?: 1);
$offset = ($page - 1) * POSTS_PER_PAGE;
$statement = $database->prepare(
    'SELECT id, title, slug, summary, body, published_at, views
     FROM posts
     ORDER BY published_at DESC, id DESC
     LIMIT :limit OFFSET :offset'
);
$statement->bindValue('limit', POSTS_PER_PAGE, PDO::PARAM_INT);
$statement->bindValue('offset', $offset, PDO::PARAM_INT);
$statement->execute();

$content = '<h2>Latest articles</h2>';
foreach ($statement as $post) {
    $content .= blog_render_post($database, $post);
}
blog_render_page('Home', $content, $database);

