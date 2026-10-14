<?php

declare(strict_types=1);

require __DIR__ . '/bootstrap.php';

$database = blog_db();
$query = trim((string)($_GET['q'] ?? 'runtime'));
$statement = $database->prepare(
    'SELECT id, title, slug, summary, body, published_at, views
     FROM posts
     WHERE title LIKE :query OR summary LIKE :query OR body LIKE :query
     ORDER BY published_at DESC
     LIMIT 20'
);
$statement->execute(['query' => '%' . $query . '%']);
$posts = $statement->fetchAll();

$content = '<h2>Search: ' . blog_escape($query) . '</h2><p>' . count($posts) . ' matching articles.</p>';
foreach ($posts as $post) {
    $content .= blog_render_post($database, $post);
}
blog_render_page('Search', $content, $database);

