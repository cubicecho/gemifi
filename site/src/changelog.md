---
layout: docs.njk
title: Changelog
description: What changed in each gemifi release, newest first.
permalink: /changelog/
crumb: Overview
tone: overview
lede: What changed in each release, newest first.
# Release notes would crowd the docs search out.
searchable: false
---

<div class="prose">

{% if changelog.latest %}The latest release is **{{ changelog.latest }}**. {% endif %}Every release is also on [GitHub]({{ site.repo }}/releases), and the Docker image is tagged with its version (`{{ site.image }}:x.y.z`).

{% if changelog.body %}
{{ changelog.body | safe }}
{% else %}
No releases yet.
{% endif %}

</div>
