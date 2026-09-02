class Structure {
  run(value) {
    const adjust = (step) => {
      for (const item of [1, 2]) {
        if (item > 0) {
          switch (item) {
            case 1:
              value += step;
              break;
            default:
              break;
          }
        }
      }
      return value;
    };
    try {
      while (value < 4) value++;
    } catch (error) {
      return 0;
    } finally {
      value++;
    }
    return adjust(1);
  }
}
