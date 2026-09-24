#pragma once

// Cropping: measuring the black border, keeping a crop per source size, and
// dragging the edges on the picture.

#include "config.h"

namespace cap {

class VideoRenderer;

class CropTool {
 public:
  // What cropping needs from the application around it.
  class Host {
   public:
    virtual ~Host() = default;
    virtual void Toast(const std::string& text) = 0;
    virtual void OpenSettings() = 0;
    virtual void CloseSettings() = 0;
  };

  CropTool(Config& config, VideoRenderer& renderer, Host& host);

  void DetectCrop();
  // Wirft den Zuschnitt weg, wenn die Quelle ihre Groesse gewechselt hat --
  // oder holt den fuer die neue Groesse gemerkten hervor.
  void UpdateCropForFormat();
  void BeginCropPick();
  void EndCropPick(bool apply);
  void DrawCropPicker();

  // Whether the edges are being dragged right now.
  bool active() const { return cropPick_.active; }

 private:
  Config& config_;
  VideoRenderer& renderer_;
  Host& host_;

  // Dragging the crop edges on the picture instead of typing four numbers.
  struct CropPick {
    bool active = false;
    ImageSettings saved;              // restored on cancel
    int left = 0, right = 0, top = 0, bottom = 0;  // source pixels
    int drag = -1;                    // 0 left, 1 right, 2 top, 3 bottom
  } cropPick_;
  // Die Quellgroesse, fuer die der gespeicherte Zuschnitt gemessen wurde.
  // 0x0 heisst: in dieser Sitzung noch kein Bild gesehen, das erste zaehlt
  // dann als der Stand, auf den sich die Zahlen beziehen. Siehe
  // UpdateCropForFormat.
  int cropFormatWidth_ = 0;
  int cropFormatHeight_ = 0;
};

}  // namespace cap
