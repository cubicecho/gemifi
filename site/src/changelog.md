---
layout: base.njk
title: Changelog
description: What changed in each gemifi release, newest first.
permalink: /changelog/
---
<section class="hero">
  <h1>Changelog</h1>
  <p>What changed in each release, newest first.{% if changelog.latest %} The latest is <strong>{{ changelog.latest }}</strong>.{% endif %}</p>
</section>

<div class="prose">

{% if changelog.body %}
{{ changelog.body | safe }}
{% else %}
No releases yet.
{% endif %}

Every release is also on [GitHub]({{ site.repo }}/releases), and the Docker image is tagged with its version (`{{ site.image }}:x.y.z`).

</div>
