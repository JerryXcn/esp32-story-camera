from app.services.charset import DISPLAYABLE, FALLBACK_TITLE, display_safe_title
from app.services.moderation import check_story_text


def test_common_title_kept():
    assert display_safe_title("小黄魔法棒的睡前探险") == "小黄魔法棒的睡前探险"


def test_rare_glyph_falls_back():
    assert display_safe_title("小狐狸与麒麟") == FALLBACK_TITLE   # 麒麟不在一级字库


def test_fallback_itself_displayable():
    assert all(ch in DISPLAYABLE for ch in FALLBACK_TITLE)
    assert len(DISPLAYABLE) > 3755


def test_check_story_text_applies_title_guard():
    title, story = check_story_text("龘龘的故事", "从前有一只小狐狸。", 2000, 8000)
    assert title == FALLBACK_TITLE and story == "从前有一只小狐狸。"


def test_long_story_trimmed_at_sentence_end():
    from app.services.moderation import trim_to_sentence
    story = "小狐狸出门了。" * 200          # 1400 字
    out = trim_to_sentence(story, 900)
    assert len(out) <= 900 and out.endswith("。")
    assert trim_to_sentence("短故事。", 900) == "短故事。"
    assert len(trim_to_sentence("没有句号" * 300, 900)) == 900
