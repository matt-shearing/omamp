// Shows a Copy button on each command block. The page works without it.
document.querySelectorAll(".cmd").forEach((block) => {
  const button = block.querySelector(".copy-btn");
  const code = block.querySelector("code");
  if (!button || !code) return;
  button.hidden = false;
  block.classList.add("has-copy");
  button.setAttribute("aria-label", "Copy these commands");
  button.addEventListener("click", async () => {
    const text = code.textContent.replace(/[ \t]+#[^\n]*/g, "");
    try {
      await navigator.clipboard.writeText(text);
      button.textContent = "Copied";
    } catch {
      const range = document.createRange();
      range.selectNodeContents(code);
      const sel = getSelection();
      sel.removeAllRanges();
      sel.addRange(range);
      button.textContent = "Selected";
    }
    setTimeout(() => { button.textContent = "Copy"; }, 1800);
  });
});
