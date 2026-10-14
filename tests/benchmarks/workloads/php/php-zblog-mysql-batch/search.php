<?php

declare(strict_types=1);

require __DIR__ . '/bootstrap.php';

$database = blog_db();
$query = trim((string)($_GET['q'] ?? 'runtime'));
$statement = $database->prepare(
    "SELECT p.id, p.title, p.slug, p.summary, p.body, p.published_at, p.views,
            COALESCE(GROUP_CONCAT(t.name ORDER BY t.name SEPARATOR ','), '') AS tag_names
     FROM posts p
     LEFT JOIN post_tags pt ON pt.post_id = p.id
     LEFT JOIN tags t ON t.id = pt.tag_id
     WHERE p.title LIKE :query OR p.summary LIKE :query OR p.body LIKE :query
     GROUP BY p.id, p.title, p.slug, p.summary, p.body, p.published_at, p.views
     ORDER BY p.published_at DESC
     LIMIT 20"
);
$statement->execute(['query' => '%' . $query . '%']);
$posts = $statement->fetchAll();

$content = '<h2>Search: ' . blog_escape($query) . '</h2><p>' . count($posts) . ' matching articles.</p>';
foreach ($posts as $post) {
    $content .= blog_render_post($post);
}
blog_render_page('Search', $content, $database);
