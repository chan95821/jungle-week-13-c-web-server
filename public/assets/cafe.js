document.addEventListener("DOMContentLoaded", () => {
  const forms = document.querySelectorAll("form[data-async-target]");
  for (const form of forms) {
    form.addEventListener("submit", async (event) => {
      event.preventDefault();
      const targetId = form.getAttribute("data-async-target");
      const container = document.getElementById(targetId);
      if (!container) return;

      const action = form.getAttribute("action") || window.location.pathname;
      const params = new URLSearchParams(new FormData(form));
      const url = `${action}?${params.toString()}`;

      try {
        const response = await fetch(url);
        const html = await response.text();
        const doc = new DOMParser().parseFromString(html, "text/html");
        const section = doc.getElementById(targetId);
        if (section) {
          container.replaceChildren(document.importNode(section, true));
        } else {
          const card = document.createElement("section");
          card.className = "result-card error";
          const msg = document.createElement("p");
          msg.textContent = `요청 처리 실패 (${response.status}): ${html.trim()}`;
          card.appendChild(msg);
          container.replaceChildren(card);
        }
      } catch (_err) {
        const card = document.createElement("section");
        card.className = "result-card error";
        const msg = document.createElement("p");
        msg.textContent = "서버와 통신하지 못했습니다.";
        card.appendChild(msg);
        container.replaceChildren(card);
      }
    });
  }
});
