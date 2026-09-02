function top() {
  function hidden() {}
}

function* generate() {
  yield 1;
}

class Widget {
  method() {
    const hidden = function named() {};
  }
}

const object = {
  objectMethod() {
    const hidden = () => {};
  },
};
