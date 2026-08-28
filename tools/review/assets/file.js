// The page a click on a file reference opens: the whole file, highlighted, anchored at the line.
//
// A separate tab rather than a panel, because following a reference should not cost you the entry you were reading.

async function main() {
  const path = decodeURIComponent(location.pathname.replace(/^\/file\//, ""));
  const line = Number((location.hash.match(/^#L(\d+)$/) || [])[1] || 0);

  document.getElementById("file-path").textContent = path;
  document.title = path.split("/").pop() + " — " + path;

  // `whole=1` asks for the file rather than a peek window: this page is where you go to read around a reference.
  const response = await fetch(`/api/file?path=${encodeURIComponent(path)}&line=0&whole=1`, {
    headers: { Accept: "application/json" },
  });
  const body = await response.json();
  if (!response.ok) {
    document.getElementById("file-body").textContent = body.error || "could not be read";
    return;
  }

  document.getElementById("file-editor").href = `vscode://file/${body.absolute}${line ? ":" + line : ""}`;

  // An image is shown, a binary is described, and only text goes through the highlighter and the gutter.
  if (body.kind === "image") {
    document.getElementById("file-meta").textContent = [body.dimensions, body.size].filter(Boolean).join(" · ");
    document.getElementById("file-body").innerHTML =
      `<span class="file-image"><img src="${escapeAttribute(body.src)}" alt="${escapeAttribute(body.path)}"></span>`;
    return;
  }
  if (body.kind === "binary") {
    document.getElementById("file-meta").textContent = body.size;
    document.getElementById("file-body").textContent = "binary file — nothing to show here";
    return;
  }

  document.getElementById("file-body").innerHTML = body.html;
  document.getElementById("file-meta").textContent = `${body.lines} lines`;

  numberLines(body.start);
  if (line) focusLine(line);
}

// The one place this page builds an attribute out of server data.
// Both values come from the index rather than from a query string, so this is belt-and-braces rather than a fix.
function escapeAttribute(text) {
  return String(text).replace(/&/g, "&amp;").replace(/"/g, "&quot;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
}

// Line numbers are added here rather than server-side: the highlighter emits one blob, and a gutter is a reading
// aid rather than part of the content.
function numberLines(start) {
  const code = document.getElementById("file-body");
  const lines = code.innerHTML.split("\n");
  // Joined with nothing rather than with a newline: `.src-line` is `display: block` and breaks the line itself,
  // while a `pre` renders the newline between two blocks as a second break.
  code.innerHTML = lines
    .map((text, index) => `<span class="src-line" id="L${start + index}" data-line="${start + index}">` +
      `<span class="src-no">${start + index}</span>${text}</span>`)
    .join("");
}

function focusLine(line) {
  const target = document.getElementById("L" + line);
  if (!target) return;
  target.classList.add("src-focus");
  target.scrollIntoView({ block: "center" });
}

main();
